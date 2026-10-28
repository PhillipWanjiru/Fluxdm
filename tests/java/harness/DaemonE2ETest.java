/* FluxDM — tests/java/harness/DaemonE2ETest.java
 * The daemon's real service classes — CoreIPCClient, QueueService, DownloadService,
 * BandwidthService, IPCBridgeService — against the REAL fluxdm-core binary and a
 * local HTTP origin that serves random files with Range support and a speed limit.
 *
 * What this adds over the other suites: the C core and the Java daemon agree, for
 * real, about the wire protocol, the order of replies, the meaning of every error,
 * resume after a pause, a core that is killed with SIGKILL, and a daemon that
 * restarts while the core keeps working. Everything is verified by comparing the
 * SHA-256 of what landed on disk with what the origin served.
 *
 * Run:  java -cp <classes> harness.DaemonE2ETest [path/to/fluxdm-core]
 *       (or -Dfluxdm.core.bin=...). Core log level: -Dfluxdm.test.corelog=0|1|2
 */
package harness;

import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.CoreIPCClient.AckResult;
import com.fluxdm.ipc.CoreIPCClient.StatusInfo;
import com.fluxdm.model.Download;
import com.fluxdm.model.Stats;
import com.fluxdm.service.BandwidthService;
import com.fluxdm.service.DownloadService;
import com.fluxdm.service.IPCBridgeService;
import com.fluxdm.service.QueueService;

import com.sun.net.httpserver.Headers;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;

import static com.fluxdm.service.DownloadService.STATUS_DONE;
import static com.fluxdm.service.DownloadService.STATUS_ERROR;
import static com.fluxdm.service.DownloadService.STATUS_PAUSED;
import static com.fluxdm.service.DownloadService.STATUS_PENDING;
import static com.fluxdm.service.DownloadService.STATUS_RUNNING;

public class DaemonE2ETest {

    static final int KiB = 1024, MiB = 1024 * 1024;

    static Path   coreBin;
    static Path   root;
    static Path   dlDir;
    static Origin origin;
    static int    sockSeq;
    static final int CORE_LOG = Integer.getInteger("fluxdm.test.corelog", 0);

    /** Cores started by hand (not by an IPCBridgeService): stopped in the end whatever happened. */
    static final List<Process> strays = new CopyOnWriteArrayList<>();

    public static void main(String[] args) throws Exception {
        String bin = System.getProperty("fluxdm.core.bin", args.length > 0 ? args[0] : "core/fluxdm-core");
        coreBin = Path.of(bin).toAbsolutePath();
        if (!Files.isExecutable(coreBin)) {
            System.err.println("fluxdm-core not found or not executable at " + coreBin + " — run `make` in core/ first");
            System.exit(2);
        }
        root  = Files.createTempDirectory(Path.of("/tmp"), "fxe");
        dlDir = Files.createDirectories(root.resolve("dl"));
        origin = new Origin();

        System.out.println("\n FluxDM — daemon end-to-end tests (real fluxdm-core: " + coreBin + ")");
        try {
            run("queueing",          DaemonE2ETest::queueing);
            run("pauseAndResume",    DaemonE2ETest::pauseAndResume);
            run("cancelling",        DaemonE2ETest::cancelling);
            run("slowProbe",         DaemonE2ETest::slowProbe);
            run("checksums",         DaemonE2ETest::checksums);
            run("httpErrors",        DaemonE2ETest::httpErrors);
            run("limits",            DaemonE2ETest::limits);
            run("coreCrash",         DaemonE2ETest::coreCrash);
            run("daemonRestart",     DaemonE2ETest::daemonRestart);
            run("bridgeLifecycle",   DaemonE2ETest::bridgeLifecycle);
        } finally {
            for (Process p : strays) if (p.isAlive()) p.destroyForcibly();
            origin.close();
            deleteTree(root);
        }
        System.exit(T.finish("Daemon end-to-end (real fluxdm-core)"));
    }

    interface Scenario { void run() throws Exception; }

    /** One scenario failing with an exception must not hide the others. */
    static void run(String name, Scenario s) {
        long t0 = System.nanoTime();
        try {
            s.run();
        } catch (Throwable t) {
            T.ok(false, name + " aborted with " + t);
            t.printStackTrace(System.out);
        }
        System.out.printf("  ·  %s: %.1fs%n", name, (System.nanoTime() - t0) / 1e9);
    }

    /* ═══ The origin: a small HTTP server with Range support and a speed limit ═══ */

    static final class Origin implements AutoCloseable {
        static final int CHUNK = 16 * KiB;
        static final Pattern RANGE = Pattern.compile("^bytes=(\\d+)-(\\d*)$");
        static final String LAST_MODIFIED = DateTimeFormatter.RFC_1123_DATE_TIME
            .format(ZonedDateTime.of(2026, 1, 2, 3, 4, 5, 0, ZoneOffset.UTC));

        static final class Cfg {
            volatile long bytesPerSec;      /* per connection; 0 = as fast as possible */
            volatile long probeDelayMs;     /* extra wait before answering the 1-byte probe */
        }

        static final class Req {
            final String path, range;
            volatile int status;
            final AtomicLong sent = new AtomicLong();
            Req(String path, String range) { this.path = path; this.range = range; }
            boolean isProbe() { return "bytes=0-0".equals(range); }
        }

        final Map<String, byte[]> files = new ConcurrentHashMap<>();
        final Map<String, Cfg>    cfg   = new ConcurrentHashMap<>();
        final List<Req>           log   = new CopyOnWriteArrayList<>();
        private final HttpServer      http;
        private final ExecutorService pool = Executors.newCachedThreadPool(r -> {
            Thread t = new Thread(r, "origin-worker");
            t.setDaemon(true);
            return t;
        });
        private final Map<String, Integer> activeByPath = new HashMap<>();
        private int maxActivePaths;
        final int port;

        Origin() throws IOException {
            http = HttpServer.create(new InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 256);
            http.setExecutor(pool);
            http.createContext("/", this::handle);
            http.start();
            port = http.getAddress().getPort();
        }

        String url(String path) { return "http://127.0.0.1:" + port + path; }

        void put(String path, byte[] data, long bytesPerSecPerConnection) {
            files.put(path, data);
            Cfg c = new Cfg();
            c.bytesPerSec = bytesPerSecPerConnection;
            cfg.put(path, c);
        }

        Cfg cfg(String path) { return cfg.computeIfAbsent(path, k -> new Cfg()); }

        /** Body bytes written for GETs of this path (the 1-byte probes included). */
        long bytesSent(String path) {
            long n = 0;
            for (Req r : log) if (r.path.equals(path)) n += r.sent.get();
            return n;
        }

        long probes(String path) {
            return log.stream().filter(r -> r.path.equals(path) && r.isProbe()).count();
        }

        synchronized int maxActivePaths() { return maxActivePaths; }

        private synchronized void enter(String path) {
            activeByPath.merge(path, 1, Integer::sum);
            maxActivePaths = Math.max(maxActivePaths, activeByPath.size());
        }

        private synchronized void leave(String path) {
            activeByPath.computeIfPresent(path, (k, v) -> v <= 1 ? null : v - 1);
        }

        private void handle(HttpExchange ex) {
            String path  = ex.getRequestURI().getPath();
            String range = ex.getRequestHeaders().getFirst("Range");
            Req r = new Req(path, range);
            log.add(r);
            boolean counted = false;
            try {
                byte[] data = files.get(path);
                if (!"GET".equals(ex.getRequestMethod())) { r.status = 405; ex.sendResponseHeaders(405, -1); return; }
                if (data == null)                         { r.status = 404; ex.sendResponseHeaders(404, -1); return; }

                Cfg c = cfg.getOrDefault(path, new Cfg());
                long len = data.length, from = 0, to = len - 1;
                int status = 200;
                if (range != null) {
                    Matcher m = RANGE.matcher(range);
                    if (!m.matches()) { r.status = 416; ex.sendResponseHeaders(416, -1); return; }
                    from = Long.parseLong(m.group(1));
                    to   = m.group(2).isEmpty() ? len - 1 : Math.min(len - 1, Long.parseLong(m.group(2)));
                    if (from >= len || from > to) {
                        ex.getResponseHeaders().set("Content-Range", "bytes */" + len);
                        r.status = 416;
                        ex.sendResponseHeaders(416, -1);
                        return;
                    }
                    status = 206;
                }
                Headers h = ex.getResponseHeaders();
                h.set("Accept-Ranges", "bytes");
                h.set("ETag", "\"" + Integer.toHexString(Arrays.hashCode(data)) + "-" + len + "\"");
                h.set("Last-Modified", LAST_MODIFIED);
                h.set("Content-Type", "application/octet-stream");
                if (status == 206) h.set("Content-Range", "bytes " + from + "-" + to + "/" + len);
                if (r.isProbe() && c.probeDelayMs > 0) T.sleep(c.probeDelayMs);

                enter(path);
                counted = true;
                long remaining = to - from + 1;
                r.status = status;
                ex.sendResponseHeaders(status, remaining);
                try (OutputStream os = ex.getResponseBody()) {
                    long off = from;
                    while (remaining > 0) {
                        int chunk = (int) Math.min(CHUNK, remaining);
                        if (chunk == remaining) { leave(path); counted = false; }   /* the client cannot finish before this chunk */
                        os.write(data, (int) off, chunk);
                        off += chunk;
                        remaining -= chunk;
                        r.sent.addAndGet(chunk);
                        long bps = c.bytesPerSec;
                        if (bps > 0 && remaining > 0) T.sleep(Math.max(1, chunk * 1000L / bps));
                    }
                }
            } catch (IOException e) {
                /* the client closed the connection: pause, cancel, crash — all normal */
            } finally {
                if (counted) leave(path);
                ex.close();
            }
        }

        @Override public void close() {
            http.stop(0);
            pool.shutdownNow();
        }
    }

    /* ═══ A daemon, wired the way Spring wires it ═══════════════════════════════ */

    static final class Daemon implements AutoCloseable {
        final Path sock;
        final MemDownloads repo;
        final CoreIPCClient ipc;
        final QueueService queue;
        final Recorder ui = new Recorder();
        final DownloadService svc;
        final BandwidthService bw;
        final IPCBridgeService bridge;

        /** @param bridgeFirst  true: the bridge connects before the services register as listeners
         *                      (Spring does not promise an order; both must work) */
        Daemon(MemDownloads repo, Path sock, int max, boolean bridgeFirst, long globalLimit) {
            this.repo  = repo;
            this.sock  = sock;
            this.ipc   = new CoreIPCClient(sock.toString());
            this.queue = new QueueService(max);
            this.svc   = new DownloadService(repo, ipc, queue, ui, true);
            this.bw    = new BandwidthService(ipc, globalLimit);
            this.bridge = new IPCBridgeService(ipc, coreBin.toString(), CORE_LOG, 8000, globalLimit, 300);
            if (bridgeFirst) {
                bridge.startup();
                ipc.addListener(svc);
                ipc.addListener(bw);
            } else {
                ipc.addListener(svc);
                ipc.addListener(bw);
                bridge.startup();
            }
        }

        static Daemon start(int max, boolean bridgeFirst) {
            return new Daemon(new MemDownloads(), freshSock(), max, bridgeFirst, 0);
        }

        boolean awaitSynced() { return T.waitFor(svc::isSynced, 10_000); }

        @Override public void close() {
            try { bridge.shutdown(); } catch (RuntimeException e) { System.out.println("  ·  bridge.shutdown: " + e); }
            ipc.close();
        }
    }

    static Path freshSock() { return root.resolve("c" + (sockSeq++) + ".sock"); }

    /** Samples the core's own view of what it is running. */
    static final class Monitor implements AutoCloseable {
        private final Daemon d;
        private volatile boolean stop;
        volatile int maxAlive;
        volatile int samples;
        private final Thread t;

        Monitor(Daemon d) {
            this.d = d;
            t = new Thread(this::loop, "e2e-monitor");
            t.setDaemon(true);
            t.start();
        }

        private void loop() {
            while (!stop) {
                try {
                    AckResult r = d.ipc.requestStatusAll().get(2, TimeUnit.SECONDS);
                    if (r.ok()) {
                        int alive = 0;
                        for (StatusInfo s : r.statuses()) if (s.status() <= CoreIPCClient.CORE_DOWNLOADING) alive++;
                        maxAlive = Math.max(maxAlive, alive);
                        samples++;
                    }
                } catch (Exception ignored) {
                    /* the core may be down on purpose */
                }
                T.sleep(25);
            }
        }

        @Override public void close() {
            stop = true;
            try { t.join(3000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
    }

    /* ═══ Helpers ═══════════════════════════════════════════════════════════════ */

    static byte[] randomBytes(long seed, int n) {
        byte[] b = new byte[n];
        new Random(seed).nextBytes(b);
        return b;
    }

    static String hex(String algorithm, byte[] data) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance(algorithm).digest(data));
    }

    static Download dl(String name) {
        Download d = new Download();
        d.setUrl(origin.url("/" + name));
        d.setFilePath(dlDir.resolve(name).toString());
        return d;
    }

    static Path file(String name)    { return dlDir.resolve(name); }
    static Path sidecar(String name) { return dlDir.resolve(name + ".fluxpart"); }

    /** The file on disk is byte-for-byte what the origin serves. */
    static boolean fileIsIntact(String name) {
        try {
            Path p = file(name);
            return Files.isRegularFile(p) && Arrays.equals(Files.readAllBytes(p), origin.files.get("/" + name));
        } catch (IOException e) {
            return false;
        }
    }

    static boolean statusIs(Daemon d, long id, int st) {
        return d.repo.findById(id).map(x -> x.getStatus() == st).orElse(false);
    }

    static boolean allDone(Daemon d, List<Download> ds) {
        for (Download x : ds) if (!statusIs(d, x.getId(), STATUS_DONE)) return false;
        return true;
    }

    static List<Integer> history(Daemon d, long id) { return d.ui.stateHistory(id); }

    static StatusInfo coreStatus(Daemon d, long id) {
        try {
            AckResult r = d.ipc.requestStatusAll().get(3, TimeUnit.SECONDS);
            if (r.ok()) for (StatusInfo s : r.statuses()) if (s.id() == id) return s;
        } catch (Exception ignored) {
            /* core away */
        }
        return null;
    }

    static long coreDownloaded(Daemon d, long id) {
        StatusInfo s = coreStatus(d, id);
        return s == null ? -1 : s.downloadedBytes();
    }

    /** Prints where every download and the core stand — only when something did not come out right. */
    static void dump(Daemon d, String why) {
        System.out.println("     ┌ state dump: " + why);
        for (Download x : d.repo.findAll())
            System.out.printf("     │ #%d %-8s %s  %s/%s  %s%n", x.getId(), DownloadService.statusName(x.getStatus()),
                              Path.of(x.getFilePath()).getFileName(), x.getDownloaded(), x.getTotalSize(),
                              x.getErrorMessage() == null ? "" : "[" + x.getErrorMessage() + "]");
        try {
            AckResult r = d.ipc.requestStatusAll().get(3, TimeUnit.SECONDS);
            for (StatusInfo s : r.statuses())
                System.out.printf("     │ core #%d state %d  %d/%d%n", s.id(), s.status(), s.downloadedBytes(), s.totalBytes());
        } catch (Exception e) {
            System.out.println("     │ core unreachable: " + e);
        }
        System.out.println("     └");
    }

    static boolean waitAllDone(Daemon d, List<Download> ds, long timeoutMs, String what) {
        boolean ok = T.within(what, timeoutMs, () -> allDone(d, ds));
        if (!ok) dump(d, what);
        return ok;
    }

    /** Changes whenever the core writes a new checkpoint (it replaces the file by renaming). */
    static String stamp(Path p) {
        try {
            BasicFileAttributes a = Files.readAttributes(p, BasicFileAttributes.class);
            return a.fileKey() + "/" + a.lastModifiedTime().toMillis();
        } catch (IOException e) {
            return "";
        }
    }

    static void deleteTree(Path dir) {
        try (Stream<Path> s = Files.walk(dir)) {
            s.sorted(Comparator.reverseOrder()).forEach(p -> { try { Files.deleteIfExists(p); } catch (IOException ignored) { /* best effort */ } });
        } catch (IOException ignored) {
            /* best effort */
        }
    }

    /* ═══ Scenarios ═════════════════════════════════════════════════════════════ */

    static void queueing() throws Exception {
        T.section("Five downloads, two slots, one real core");
        final int size = 3 * MiB;
        for (int i = 1; i <= 5; i++) origin.put("/q" + i + ".bin", randomBytes(100 + i, size), 200 * KiB);

        try (Daemon d = Daemon.start(2, true); Monitor mon = new Monitor(d)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the freshly spawned core (bridge connected before the services registered)");
            T.eq(Boolean.TRUE, d.bridge.status().get("spawnedByDaemon"), "the core was spawned by the daemon");

            List<Download> all = new ArrayList<>();
            for (int i = 1; i <= 5; i++) all.add(d.svc.start(dl("q" + i + ".bin")));
            T.eq(STATUS_RUNNING, all.get(0).getStatus(), "#1 is running as soon as start() returns");
            T.eq(STATUS_RUNNING, all.get(1).getStatus(), "#2 is running");
            T.eq(STATUS_PENDING, all.get(2).getStatus(), "#3 waits for a slot");
            T.eq(STATUS_PENDING, all.get(4).getStatus(), "#5 waits too");
            Stats st = d.svc.getStats();
            T.ok(st.getActive() == 2 && st.getQueued() == 3 && st.getTotal() == 5,
                 "stats: 2 active, 3 queued, 5 total (" + st.getActive() + "/" + st.getQueued() + "/" + st.getTotal() + ")");

            waitAllDone(d, all, 90_000, "all five downloads complete");
            for (int i = 1; i <= 5; i++) T.ok(fileIsIntact("q" + i + ".bin"), "q" + i + ".bin is byte-for-byte what the origin served");
            T.ok(mon.maxAlive <= 2, "the core never ran more than 2 at once (most it ran: " + mon.maxAlive + ", " + mon.samples + " samples)");
            T.eq(2, mon.maxAlive, "...and it did run 2 together, so that check meant something");
            T.ok(origin.maxActivePaths() <= 2, "the origin never served more than 2 files at once (" + origin.maxActivePaths() + ")");
            T.eq(0, d.queue.activeCount(), "no slot left held");
            T.eq(0, d.queue.queuedCount(), "nothing left waiting");
            boolean tidy = true, sizes = true, announced = true;
            for (Download x : all) {
                String name = Path.of(x.getFilePath()).getFileName().toString();
                tidy &= !Files.exists(sidecar(name));
                Download row = d.repo.findById(x.getId()).orElseThrow();
                sizes &= row.getTotalSize() == size && row.getDownloaded() == size;
                announced &= d.ui.of("complete", x.getId()).size() == 1;
            }
            T.ok(tidy, "no .fluxpart checkpoint left behind");
            T.ok(sizes, "the database holds the final size and progress of every download");
            T.ok(announced, "each completion was announced to the UI exactly once");
            T.eq(5L, d.svc.getStats().getComplete(), "stats: 5 complete");
        }
    }

    static void pauseAndResume() throws Exception {
        T.section("Pause and resume against the real core");
        final int size = 4 * MiB;
        origin.put("/p1.bin", randomBytes(201, size), 128 * KiB);

        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");
            Download a = d.svc.start(dl("p1.bin"));
            final long id = a.getId();
            T.within("a quarter of the file has arrived", 30_000, () -> coreDownloaded(d, id) >= size / 4);

            Download paused = d.svc.pause(id);
            T.eq(STATUS_PAUSED, paused.getStatus(), "pause() returns the download PAUSED");
            StatusInfo cs = coreStatus(d, id);
            T.ok(cs != null && cs.status() == CoreIPCClient.CORE_PAUSED, "the core says PAUSED too");
            T.ok(Files.exists(sidecar("p1.bin")), "the resume checkpoint is on disk");
            T.eq(0, d.queue.activeCount(), "the slot was given back");
            long frozen = coreDownloaded(d, id);
            T.sleep(600);
            T.eq(frozen, coreDownloaded(d, id), "the transfer really stopped: the core's byte count did not move in 600 ms");
            T.eq(STATUS_PAUSED, d.svc.pause(id).getStatus(), "pausing twice is harmless");

            Download resumed = d.svc.resume(id);
            T.eq(STATUS_RUNNING, resumed.getStatus(), "resume() returns the download RUNNING");
            T.eq(STATUS_RUNNING, d.svc.resume(id).getStatus(), "resuming twice is harmless");
            waitAllDone(d, List.of(a), 60_000, "the download completes");
            T.ok(fileIsIntact("p1.bin"), "the file is byte-for-byte what the origin served");
            long sent = origin.bytesSent("/p1.bin");
            T.ok(sent < size * 13L / 10, "it resumed instead of starting over: the origin sent " + sent + " bytes for a " + size + " byte file");
            T.eq(List.of(STATUS_RUNNING, STATUS_PAUSED, STATUS_PENDING, STATUS_RUNNING, STATUS_DONE), history(d, id),
                 "the UI saw running, paused, pending, running, done — in that order");
        }
    }

    static void cancelling() throws Exception {
        T.section("Cancel against the real core");
        final int size = 3 * MiB;
        origin.put("/c1.bin", randomBytes(301, size), 96 * KiB);
        origin.put("/c2.bin", randomBytes(302, size), 96 * KiB);
        origin.put("/c3.bin", randomBytes(303, 256 * KiB), 0);
        origin.put("/c4.bin", randomBytes(304, size), 96 * KiB);

        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");

            /* running */
            Download c1 = d.svc.start(dl("c1.bin"));
            T.within("c1 is downloading", 30_000, () -> coreDownloaded(d, c1.getId()) > 128 * KiB);
            d.svc.cancel(c1.getId());
            T.ok(d.repo.findById(c1.getId()).isEmpty(), "a running download's row is gone");
            T.within("its data and checkpoint are deleted", 5000, () -> !Files.exists(file("c1.bin")) && !Files.exists(sidecar("c1.bin")));
            T.ok(coreStatus(d, c1.getId()) == null, "the core forgot it");
            T.eq(0, d.queue.activeCount(), "its slot is free");

            /* paused */
            Download c2 = d.svc.start(dl("c2.bin"));
            T.within("c2 is downloading", 30_000, () -> coreDownloaded(d, c2.getId()) > 128 * KiB);
            d.svc.pause(c2.getId());
            T.ok(Files.exists(sidecar("c2.bin")), "paused: the checkpoint exists");
            d.svc.cancel(c2.getId());
            T.ok(d.repo.findById(c2.getId()).isEmpty(), "a paused download's row is gone");
            T.within("its data and checkpoint are deleted", 5000, () -> !Files.exists(file("c2.bin")) && !Files.exists(sidecar("c2.bin")));

            /* complete: the finished file is never touched */
            Download c3 = d.svc.start(dl("c3.bin"));
            waitAllDone(d, List.of(c3), 30_000, "c3 completes");
            d.svc.cancel(c3.getId());
            T.ok(d.repo.findById(c3.getId()).isEmpty(), "a finished download's row is gone");
            T.ok(fileIsIntact("c3.bin"), "...and its file is left exactly as it was");

            /* waiting for a slot */
            Download w1 = d.svc.start(dl("c1.bin"));
            Download w2 = d.svc.start(dl("c2.bin"));
            Download w3 = d.svc.start(dl("c4.bin"));
            T.eq(STATUS_PENDING, w3.getStatus(), "the third waits (two slots)");
            d.svc.cancel(w3.getId());
            T.ok(d.repo.findById(w3.getId()).isEmpty(), "a waiting download's row is gone");
            T.eq(2, d.queue.activeCount(), "the two running ones still hold their slots");
            T.eq(0, d.queue.queuedCount(), "nobody is waiting any more");
            d.svc.cancel(w1.getId());
            d.svc.cancel(w2.getId());
            T.ok(!Files.exists(file("c4.bin")) && !Files.exists(sidecar("c4.bin")), "nothing was ever created for the one that never started");
            T.within("everything is cleaned up", 5000, () -> !Files.exists(file("c1.bin")) && !Files.exists(file("c2.bin")));
            T.eq(0, d.queue.activeCount(), "no slot held");
            T.eq(0, d.queue.queuedCount(), "nothing waiting");
        }
    }

    static void slowProbe() throws Exception {
        T.section("Pause and cancel while the core is still probing the server");
        origin.put("/s1.bin", randomBytes(401, 1 * MiB), 0);
        origin.put("/s2.bin", randomBytes(402, 1 * MiB), 0);
        origin.cfg("/s1.bin").probeDelayMs = 1200;
        origin.cfg("/s2.bin").probeDelayMs = 1200;

        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");

            Download s1 = d.svc.start(dl("s1.bin"));
            Download p = d.svc.pause(s1.getId());          /* the origin is still holding the probe back */
            T.eq(STATUS_PAUSED, p.getStatus(), "pause() during the probe returns PAUSED");
            T.within("the core settles in PAUSED once the probe is over", 8000, () -> {
                StatusInfo s = coreStatus(d, s1.getId());
                return s != null && s.status() == CoreIPCClient.CORE_PAUSED;
            });
            T.eq(0, d.queue.activeCount(), "no slot held");
            Download r = d.svc.resume(s1.getId());
            T.eq(STATUS_RUNNING, r.getStatus(), "resume() returns RUNNING");
            waitAllDone(d, List.of(s1), 30_000, "the download completes");
            T.ok(fileIsIntact("s1.bin"), "the file is byte-for-byte what the origin served");

            Download s2 = d.svc.start(dl("s2.bin"));
            d.svc.cancel(s2.getId());                      /* cancelled before the probe answered */
            T.ok(d.repo.findById(s2.getId()).isEmpty(), "cancel() during the probe removes the row");
            T.sleep(1800);                                  /* long enough for the probe to come back */
            T.ok(!Files.exists(file("s2.bin")) && !Files.exists(sidecar("s2.bin")), "nothing was written for it");
            T.ok(coreStatus(d, s2.getId()) == null, "the core does not know it any more");
            T.eq(0, d.queue.activeCount(), "no slot held");
        }
    }

    static void checksums() throws Exception {
        T.section("Checksums, verified by the core");
        byte[] a = randomBytes(501, 768 * KiB), b = randomBytes(502, 512 * KiB), c = randomBytes(503, 512 * KiB);
        origin.put("/k1.bin", a, 0);
        origin.put("/k2.bin", b, 0);
        origin.put("/k3.bin", c, 0);

        try (Daemon d = Daemon.start(4, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");

            Download good = dl("k1.bin");
            good.setChecksumType(2);
            good.setChecksum(hex("SHA-256", a).toUpperCase());     /* upper case: normalised by the daemon */
            Download g = d.svc.start(good);

            Download bad = dl("k2.bin");
            bad.setChecksumType(2);
            bad.setChecksum("00".repeat(32));
            Download bd = d.svc.start(bad);

            Download md5 = dl("k3.bin");
            md5.setChecksumType(1);
            md5.setChecksum(hex("MD5", c));
            Download m = d.svc.start(md5);

            waitAllDone(d, List.of(g, m), 30_000, "the downloads with a matching checksum complete");
            T.ok(fileIsIntact("k1.bin") && fileIsIntact("k3.bin"), "both files are intact");
            T.eq(1, d.ui.of("complete", g.getId()).get(0).get("checksumOk"), "the completion says the checksum was verified");

            T.within("the download with the wrong checksum fails", 30_000, () -> statusIs(d, bd.getId(), STATUS_ERROR));
            Download row = d.repo.findById(bd.getId()).orElseThrow();
            T.ok(row.getErrorMessage() != null && row.getErrorMessage().toLowerCase().contains("checksum"),
                 "...with the core's explanation (" + row.getErrorMessage() + ")");
            T.within("the corrupt file was removed", 5000, () -> !Files.exists(file("k2.bin")));
            T.eq(0, d.queue.activeCount(), "its slot was given back");
            T.ok(d.ui.of("error", bd.getId()).size() == 1, "the UI was told once");
        }
    }

    static void httpErrors() throws Exception {
        T.section("A 404, and a retry once the file exists");
        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");
            Download n = d.svc.start(dl("missing.bin"));
            T.within("the download fails", 30_000, () -> statusIs(d, n.getId(), STATUS_ERROR));
            Download row = d.repo.findById(n.getId()).orElseThrow();
            String msg = String.valueOf(row.getErrorMessage()).toLowerCase();
            T.ok(msg.contains("404") || msg.contains("not found"), "the error says what happened (" + row.getErrorMessage() + ")");
            T.eq(0, d.queue.activeCount(), "the slot was given back");
            T.ok(!Files.exists(file("missing.bin")), "no file was created");

            byte[] data = randomBytes(601, 600 * KiB);
            origin.put("/missing.bin", data, 0);
            Download again = d.svc.resume(n.getId());
            T.ok(again.getStatus() == STATUS_RUNNING || again.getStatus() == STATUS_PENDING, "resume() of a failed download starts it again");
            T.eq(null, again.getErrorMessage(), "the old error text is cleared");
            waitAllDone(d, List.of(n), 30_000, "it completes now that the file exists");
            T.ok(fileIsIntact("missing.bin"), "the file is intact");
        }
    }

    static void limits() throws Exception {
        T.section("Speed limits reach the core");
        final int size = 6 * MiB;
        origin.put("/l1.bin", randomBytes(701, size), 256 * KiB);     /* 8 connections: about 2 MiB/s unlimited */

        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");
            Download a = d.svc.start(dl("l1.bin"));
            T.within("it is downloading", 30_000, () -> coreDownloaded(d, a.getId()) > 256 * KiB);
            Download limited = d.svc.setLimit(a.getId(), 300_000);
            T.eq(300_000L, limited.getBandwidthLimit(), "the limit is stored");
            T.sleep(800);                                                   /* let the limiter settle */
            long d0 = coreDownloaded(d, a.getId());
            long t0 = System.nanoTime();
            T.sleep(2000);
            long d1 = coreDownloaded(d, a.getId());
            double bps = (d1 - d0) / ((System.nanoTime() - t0) / 1e9);
            T.ok(bps > 100_000 && bps < 450_000, "the core held the download to about 300 kB/s (measured " + (long) bps + " B/s)");
            d.svc.setLimit(a.getId(), 0);
            T.within("unlimited again: it finishes", 60_000, () -> statusIs(d, a.getId(), STATUS_DONE));
            T.ok(fileIsIntact("l1.bin"), "the file is intact");

            d.bw.setGlobalLimit(5_000_000);
            T.eq(5_000_000L, d.bw.getGlobalLimit(), "the global limit is accepted by the core and remembered");
            d.bw.setGlobalLimit(0);
        }
    }

    static void coreCrash() throws Exception {
        T.section("The core is killed with SIGKILL in the middle of a transfer");
        final int size = 4 * MiB;
        origin.put("/x1.bin", randomBytes(801, size), 64 * KiB);
        origin.put("/x2.bin", randomBytes(802, size), 64 * KiB);
        origin.put("/x3.bin", randomBytes(803, 1 * MiB), 0);

        try (Daemon d = Daemon.start(2, false)) {
            T.ok(d.awaitSynced(), "the daemon is in step with the core");
            Download x1 = d.svc.start(dl("x1.bin"));
            Download x2 = d.svc.start(dl("x2.bin"));
            Download x3 = d.svc.start(dl("x3.bin"));
            T.eq(STATUS_PENDING, x3.getStatus(), "the third waits for a slot");

            T.within("both have passed the halfway mark", 40_000,
                     () -> coreDownloaded(d, x1.getId()) >= size / 2 && coreDownloaded(d, x2.getId()) >= size / 2);
            /* Kill right after a checkpoint, so how much is re-fetched afterwards shows whether it resumed. */
            String before = stamp(sidecar("x1.bin"));
            T.within("a fresh checkpoint was written", 8000, () -> !stamp(sidecar("x1.bin")).equals(before));
            T.sleep(150);

            long pid = (Long) d.bridge.status().get("pid");
            ProcessHandle.of(pid).orElseThrow().destroyForcibly();
            T.within("the daemon notices", 5000, () -> !d.svc.isSynced());
            T.within("the supervisor restarted the core and the daemon is in step again", 30_000, d.svc::isSynced);
            Object newPid = d.bridge.status().get("pid");
            T.ok(newPid != null && (Long) newPid != pid, "it is a new process (" + pid + " -> " + newPid + ")");
            T.eq(1, d.bridge.status().get("restarts"), "one restart was counted");

            waitAllDone(d, List.of(x1, x2, x3), 90_000, "all three downloads complete");
            T.ok(fileIsIntact("x1.bin") && fileIsIntact("x2.bin") && fileIsIntact("x3.bin"), "all three files are byte-for-byte what the origin served");
            long s1 = origin.bytesSent("/x1.bin"), s2 = origin.bytesSent("/x2.bin");
            T.ok(s1 < size * 15L / 10 && s2 < size * 15L / 10,
                 "they resumed from their checkpoints: the origin sent " + s1 + " and " + s2 + " bytes for " + size + " byte files");
            T.ok(!history(d, x1.getId()).contains(STATUS_ERROR) && !history(d, x2.getId()).contains(STATUS_ERROR) && !history(d, x3.getId()).contains(STATUS_ERROR),
                 "no download was ever reported failed");
            T.eq(0, d.queue.activeCount(), "no slot left held");
            T.eq(0, d.queue.queuedCount(), "nothing left waiting");
            T.eq(1L, origin.probes("/x3.bin"), "the waiting one was probed once: it started once, after a slot freed up");
        }
    }

    static void daemonRestart() throws Exception {
        T.section("The daemon restarts; the core carries on and is adopted");
        final int size = 6 * MiB;
        origin.put("/r1.bin", randomBytes(901, size), 128 * KiB);
        origin.put("/r2.bin", randomBytes(902, size), 128 * KiB);
        origin.put("/r3.bin", randomBytes(903, 1 * MiB), 0);

        /* A core that belongs to nobody in particular — as if the previous daemon had started it. */
        Path sock = freshSock();
        Process core = new ProcessBuilder(coreBin.toString(), "--sock", sock.toString(), "--log", String.valueOf(CORE_LOG))
            .redirectOutput(ProcessBuilder.Redirect.DISCARD)
            .redirectError(ProcessBuilder.Redirect.INHERIT)
            .start();
        strays.add(core);
        T.ok(T.waitFor(() -> canConnect(sock), 5000), "a core is listening");

        MemDownloads db = new MemDownloads();                     /* the database outlives both daemons */
        Download r1, r2, r3;
        Daemon first = new Daemon(db, sock, 2, true, 0);
        try {
            T.ok(first.awaitSynced(), "daemon #1 adopted the running core");
            T.eq(Boolean.FALSE, first.bridge.status().get("spawnedByDaemon"), "...and knows it did not start it");
            r1 = first.svc.start(dl("r1.bin"));
            r2 = first.svc.start(dl("r2.bin"));
            r3 = first.svc.start(dl("r3.bin"));
            T.eq(STATUS_PENDING, r3.getStatus(), "the third waits for a slot");
            final long id1 = r1.getId(), id2 = r2.getId();
            T.within("both are a quarter done", 30_000, () -> coreDownloaded(first, id1) >= size / 4 && coreDownloaded(first, id2) >= size / 4);
        } finally {
            first.close();                                       /* the daemon goes away (an adopted core is left running) */
        }
        T.ok(core.isAlive(), "the core outlives the daemon");
        long sent0 = origin.bytesSent("/r1.bin");
        T.sleep(600);
        T.ok(origin.bytesSent("/r1.bin") - sent0 > 100 * KiB, "downloads continue while no daemon is attached (" + (origin.bytesSent("/r1.bin") - sent0) + " bytes in 600 ms)");

        Daemon second = new Daemon(db, sock, 2, true, 0);
        try {
            T.ok(second.awaitSynced(), "daemon #2 adopted the same core");
            T.eq(Boolean.FALSE, second.bridge.status().get("spawnedByDaemon"), "it did not spawn another one");
            T.eq(STATUS_RUNNING, db.findById(r1.getId()).orElseThrow().getStatus(), "r1 is RUNNING — taken over, not restarted");
            T.eq(STATUS_RUNNING, db.findById(r2.getId()).orElseThrow().getStatus(), "r2 is RUNNING");
            T.eq(2, second.queue.activeCount(), "both hold a slot");
            T.eq(List.of(r3.getId()), second.queue.waitingIds(), "r3 is waiting for one");

            final List<Download> all = List.of(r1, r2, r3);
            waitAllDone(second, all, 90_000, "all three complete");
            T.ok(fileIsIntact("r1.bin") && fileIsIntact("r2.bin") && fileIsIntact("r3.bin"), "all three files are byte-for-byte what the origin served");
            T.eq(1L, origin.probes("/r1.bin"), "r1 was started once (no second START behind the core's back)");
            T.eq(1L, origin.probes("/r2.bin"), "r2 was started once");
            boolean clean = true;
            for (Download x : all) clean &= !history(second, x.getId()).contains(STATUS_ERROR);
            T.ok(clean, "daemon #2 never reported a failure");
            T.eq(0, second.queue.activeCount(), "no slot left held");

            AckResult bye = second.ipc.shutdownCore().get(5, TimeUnit.SECONDS);
            T.ok(bye.ok(), "the adopted core can still be told to shut down on request");
            T.ok(core.waitFor(5, TimeUnit.SECONDS), "...and it exits");
        } finally {
            second.close();
        }
    }

    static boolean canConnect(Path sock) {
        try (SocketChannel c = SocketChannel.open(StandardProtocolFamily.UNIX)) {
            c.connect(UnixDomainSocketAddress.of(sock));
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    static void bridgeLifecycle() throws Exception {
        T.section("Starting and stopping the core");

        /* Things that must fail loudly instead of leaving a daemon that looks fine. */
        IPCBridgeService missing = new IPCBridgeService(new CoreIPCClient(freshSock().toString()), root.resolve("nope/fluxdm-core").toString(), 0, 1000, 0, 300);
        Throwable t1 = thrown(missing::startup);
        T.ok(t1 instanceof IllegalStateException && String.valueOf(t1.getCause()).contains("not found"), "a missing binary stops the daemon with a clear message (" + t1 + " / " + (t1 == null ? null : t1.getCause()) + ")");

        Path notExec = root.resolve("not-executable");
        Files.writeString(notExec, "#!/bin/sh\nexit 0\n");
        Files.setPosixFilePermissions(notExec, PosixFilePermissions.fromString("rw-r--r--"));
        IPCBridgeService noexec = new IPCBridgeService(new CoreIPCClient(freshSock().toString()), notExec.toString(), 0, 1000, 0, 300);
        Throwable t2 = thrown(noexec::startup);
        T.ok(t2 instanceof IllegalStateException && String.valueOf(t2.getCause()).contains("not executable"), "a binary that is not executable is reported as such (" + (t2 == null ? null : t2.getCause()) + ")");

        Path dies = root.resolve("dies-at-once");
        Files.writeString(dies, "#!/bin/sh\nexit 3\n");
        Files.setPosixFilePermissions(dies, PosixFilePermissions.fromString("rwxr-xr-x"));
        IPCBridgeService quits = new IPCBridgeService(new CoreIPCClient(freshSock().toString()), dies.toString(), 0, 3000, 0, 300);
        long t0 = System.nanoTime();
        Throwable t3 = thrown(quits::startup);
        T.ok(t3 instanceof IllegalStateException && String.valueOf(t3.getCause()).contains("exited immediately"),
             "a core that exits at once is reported with its exit code (" + (t3 == null ? null : t3.getCause()) + ")");
        T.ok((System.nanoTime() - t0) / 1e9 < 2.5, "...without waiting for the whole startup timeout");

        Path hangs = root.resolve("never-listens");
        Files.writeString(hangs, "#!/bin/sh\nexec sleep 30\n");
        Files.setPosixFilePermissions(hangs, PosixFilePermissions.fromString("rwxr-xr-x"));
        IPCBridgeService stuck = new IPCBridgeService(new CoreIPCClient(freshSock().toString()), hangs.toString(), 0, 800, 0, 300);
        Throwable t4 = thrown(stuck::startup);
        T.ok(t4 instanceof IllegalStateException && String.valueOf(t4.getCause()).contains("did not start listening"),
             "a core that never starts listening is given up on and killed (" + (t4 == null ? null : t4.getCause()) + ")");

        /* The settings really reach the core's command line (a wrapper records its arguments, then
         * becomes the real core). Out-of-range patience is clamped, not passed on to make the core refuse to start. */
        Path argsFile = root.resolve("core-args.txt");
        Path wrapper = root.resolve("core-wrapper");
        Files.writeString(wrapper, "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + argsFile + "'\nexec '" + coreBin + "' \"$@\"\n");
        Files.setPosixFilePermissions(wrapper, PosixFilePermissions.fromString("rwxr-xr-x"));
        for (int[] c : new int[][] {{45, 45}, {0, 0}, {-5, 0}, {1_000_000, 86_400}}) {
            Files.deleteIfExists(argsFile);
            Path wsock = freshSock();
            CoreIPCClient wipc = new CoreIPCClient(wsock.toString());
            IPCBridgeService wrapped = new IPCBridgeService(wipc, wrapper.toString(), 0, 8000, 123456, c[0]);
            try {
                wrapped.startup();
                List<String> got = Files.exists(argsFile) ? Files.readAllLines(argsFile) : List.of();
                T.eq(List.of("--sock", wsock.toString(), "--log", "0", "--limit", "123456", "--patience", String.valueOf(c[1])), got,
                     "patience-seconds=" + c[0] + " reaches the core as --patience " + c[1] + " (with the other settings)");
            } finally {
                wrapped.shutdown();
                wipc.close();
            }
        }

        /* A healthy start and a clean stop. */
        Daemon d = Daemon.start(2, true);
        T.ok(d.awaitSynced(), "a core is spawned, connected and reconciled");
        Map<String, Object> st = d.bridge.status();
        long pid = (Long) st.get("pid");
        T.ok(d.bridge.isCoreAlive() && Boolean.TRUE.equals(st.get("spawnedByDaemon")), "status: connected and spawned by the daemon (pid " + pid + ")");
        T.eq(0, st.get("restarts"), "no restarts so far");
        T.ok(canConnect(d.sock), "the socket accepts connections");
        long q0 = System.nanoTime();
        d.close();
        T.ok(!ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false), "closing the daemon stopped the core it spawned (" + (System.nanoTime() - q0) / 1_000_000 + " ms)");
        T.ok(!canConnect(d.sock), "...and nobody listens on the socket any more");

        /* The core that the daemon did not start is left alone. */
        Path sock = freshSock();
        Process core = new ProcessBuilder(coreBin.toString(), "--sock", sock.toString(), "--log", String.valueOf(CORE_LOG))
            .redirectOutput(ProcessBuilder.Redirect.DISCARD).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        strays.add(core);
        T.ok(T.waitFor(() -> canConnect(sock), 5000), "another core is listening");
        Daemon adopter = new Daemon(new MemDownloads(), sock, 2, false, 0);
        T.ok(adopter.awaitSynced(), "adopted");
        adopter.close();
        T.sleep(300);
        T.ok(core.isAlive() && canConnect(sock), "closing the daemon left the adopted core running");
        core.destroyForcibly();
        core.waitFor(5, TimeUnit.SECONDS);
    }

    static Throwable thrown(Runnable r) {
        try { r.run(); return null; } catch (Throwable t) { return t; }
    }
}
