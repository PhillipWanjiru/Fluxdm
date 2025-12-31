/* FluxDM — tests/java/harness/DownloadServiceTest.java
 * DownloadService + QueueService + the real CoreIPCClient, against FakeCore and an
 * in-memory repository: validation, concurrency limit, queue order, pause/resume/
 * cancel, asynchronous errors, recovery after the core or the daemon restarts. */
package harness;

import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.model.Download;
import com.fluxdm.model.ScheduleTask;
import com.fluxdm.service.BandwidthService;
import com.fluxdm.service.DownloadException;
import com.fluxdm.service.DownloadException.Kind;
import com.fluxdm.service.DownloadService;
import com.fluxdm.service.QueueService;
import com.fluxdm.service.SchedulerService;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

import static com.fluxdm.service.DownloadService.STATUS_DONE;
import static com.fluxdm.service.DownloadService.STATUS_ERROR;
import static com.fluxdm.service.DownloadService.STATUS_PAUSED;
import static com.fluxdm.service.DownloadService.STATUS_PENDING;
import static com.fluxdm.service.DownloadService.STATUS_RUNNING;

public class DownloadServiceTest {

    static int sockSeq;

    static final class Env implements AutoCloseable {
        final Path sock = Path.of("/tmp/fxs" + ProcessHandle.current().pid() + "_" + (sockSeq++) + ".sock");
        final FakeCore core;
        final CoreIPCClient ipc;
        final MemDownloads repo = new MemDownloads();
        final Recorder ui = new Recorder();
        final QueueService queue;
        final DownloadService svc;

        Env(int max) throws Exception { this(max, true, null, null, true); }

        /** @param beforeConnect  runs after everything exists but before the client connects
         *                        (the database / core as they were when the daemon started) */
        Env(int max, boolean autoResume, Consumer<Env> beforeConnect, Consumer<FakeCore> coreSetup, boolean waitSynced) throws Exception {
            core = new FakeCore(sock);
            if (coreSetup != null) coreSetup.accept(core);
            ipc = new CoreIPCClient(sock.toString());
            ipc.setAckTimeoutMillis(2000);
            queue = new QueueService(max);
            svc = new DownloadService(repo, ipc, queue, ui, autoResume);
            ipc.addListener(svc);
            if (beforeConnect != null) beforeConnect.accept(this);
            ipc.connect();
            if (waitSynced) T.ok(T.waitFor(svc::isSynced, 5000), "daemon is in step with the core after connecting");
        }

        @Override public void close() { ipc.close(); core.close(); }

        Download start(String name) { return svc.start(req(name)); }
        Download start(String name, int priority) { Download d = req(name); d.setPriority(priority); return svc.start(d); }
        int status(long id) { return repo.status(id); }
        boolean statusIs(long id, int st) { return repo.findById(id).map(d -> d.getStatus() == st).orElse(false); }
        void waitStatus(long id, int st, String why) { T.within(why, 4000, () -> statusIs(id, st)); }
    }

    static Download req(String name) {
        Download d = new Download();
        d.setUrl("http://127.0.0.1:9/" + name);
        d.setFilePath("/tmp/fxsvc/" + name);
        return d;
    }

    /** A database row as it was before the daemon started. id 0 = let the repository assign the next one. */
    static Download row(long id, String name, int status, int priority) {
        Download d = req(name);
        d.setId(id == 0 ? null : id);
        d.setStatus(status);
        d.setPriority(priority);
        return d;
    }

    static Kind kindOf(Runnable r) {
        try { r.run(); return null; }
        catch (DownloadException e) { return e.kind(); }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM — DownloadService / QueueService / CoreIPCClient tests");

        validation();
        startAndLimit();
        priorityOrder();
        slotsAreReleasedOnce();
        pausing();
        resuming();
        cancelling();
        asyncErrorsAndCompletion();
        syncStartRejection();
        progressHandling();
        coreUnreachable();
        reconcileAfterRestart();
        reconcileAdoptsRunningWork();
        reconcileCorrections();
        notSyncedUntilReconciled();
        reconnectAfterCoreRestart();
        limits();
        commandsDuringLaunch();
        lateAcknowledgement();
        timeoutThenAdopt();
        scheduler();
        bandwidth();

        System.exit(T.finish("DownloadService"));
    }

    /* ─────────────────────────────────────────────────────────────────── */

    static void validation() throws Exception {
        T.section("Validation: what a client may and may not say");
        try (Env e = new Env(2)) {
            String[][] bad = {
                { "no URL",              null,                         "/tmp/x" },
                { "blank URL",           "   ",                        "/tmp/x" },
                { "ftp scheme",          "ftp://h/x",                  "/tmp/x" },
                { "no scheme",           "example.com/x",              "/tmp/x" },
                { "URL with a space",    "http://h/a b",               "/tmp/x" },
                { "URL without a host",  "http:///x",                  "/tmp/x" },
                { "URL control char",    "http://h/a\u0001b",          "/tmp/x" },
                { "overlong URL",        "http://h/" + "a".repeat(4100), "/tmp/x" },
                { "no path",             "http://h/x",                 null },
                { "relative path",       "http://h/x",                 "downloads/x" },
                { "directory path",      "http://h/x",                 "/tmp/dir/" },
                { "root path",           "http://h/x",                 "/" },
                { "path control char",   "http://h/x",                 "/tmp/a\u0000b" },
                { "overlong path",       "http://h/x",                 "/" + "p".repeat(1100) },
            };
            for (String[] b : bad) {
                final Download bd = new Download();
                bd.setUrl(b[1]); bd.setFilePath(b[2]);
                T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(bd)), "rejected: " + b[0]);
            }
            final Download d1 = req("c1"); d1.setChecksumType(2); d1.setChecksum("abc123");
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(d1)), "rejected: sha256 checksum of the wrong length");
            final Download dz = req("c2"); dz.setChecksumType(1); dz.setChecksum("z".repeat(32));
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(dz)), "rejected: md5 checksum that is not hex");
            final Download dn = req("c3"); dn.setChecksumType(2); dn.setChecksum(null);
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(dn)), "rejected: checksum type without a checksum");
            final Download d4 = req("c4"); d4.setChecksumType(3);
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(d4)), "rejected: unknown checksum type");
            final Download d5 = req("c5"); d5.setBandwidthLimit(-1L);
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(d5)), "rejected: negative bandwidth limit");
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.start(null)), "rejected: no body");
            T.eq(0L, e.repo.count(), "none of them created a row");
            T.eq(0, e.core.commands(FakeCore.START).size(), "...and none reached the core");

            /* Accepted, with normalisation. */
            Download good = req("norm.bin");
            good.setFilePath("/tmp/fxsvc/../fxsvc/./sub/norm.bin");
            good.setSegments(500);
            good.setPriority(99);
            good.setChecksumType(2);
            good.setChecksum("  " + "AB".repeat(32) + " ");
            good.setId(999L);                      /* client-supplied noise that must not survive */
            good.setStatus(STATUS_DONE);
            good.setDownloaded(12345L);
            good.setTotalSize(777L);
            good.setErrorMessage("forged");
            Download saved = e.svc.start(good);
            T.ok(saved.getId() != null && saved.getId() != 999L, "the id comes from the database, not from the client (" + saved.getId() + ")");
            T.eq("/tmp/fxsvc/sub/norm.bin", saved.getFilePath(), "path normalised");
            T.eq(64, saved.getSegments(), "segments clamped to 64");
            T.eq(10, saved.getPriority(), "priority clamped to 10");
            T.eq("ab".repeat(32), saved.getChecksum(), "checksum stripped and lower-cased");
            T.eq(0L, saved.getDownloaded(), "client-supplied progress ignored");
            T.eq(null, saved.getErrorMessage(), "client-supplied error text ignored");
            T.eq(STATUS_RUNNING, saved.getStatus(), "and it started (status RUNNING, not the forged DONE)");

            Download tilde = req("t.bin"); tilde.setFilePath("~/Downloads/t.bin"); tilde.setSegments(0); tilde.setPriority(0);
            Download st = e.svc.start(tilde);
            T.eq(System.getProperty("user.home") + "/Downloads/t.bin", st.getFilePath(), "~/ expands to the home directory");
            T.eq(8, st.getSegments(), "segments 0 means the default (8)");
            T.eq(1, st.getPriority(), "priority below 1 raised to 1");

            Download dup = req("again.bin"); dup.setFilePath("/tmp/fxsvc/sub/norm.bin");
            T.eq(Kind.CONFLICT, kindOf(() -> e.svc.start(dup)), "a second download into a file that is in use is refused (409)");
        }
    }

    static void startAndLimit() throws Exception {
        T.section("START on the wire, and the concurrency limit");
        try (Env e = new Env(2)) {
            Download d = req("a.bin");
            d.setSegments(5); d.setBandwidthLimit(250_000L); d.setChecksumType(1); d.setChecksum("0123456789abcdef0123456789abcdef");
            Download a = e.svc.start(d);
            T.eq(STATUS_RUNNING, a.getStatus(), "start() returns the download already RUNNING (the core's OK was awaited)");
            FakeCore.Cmd c = e.core.commands(FakeCore.START, a.getId()).get(0);
            ByteBuffer b = ByteBuffer.wrap(c.payload()).order(ByteOrder.LITTLE_ENDIAN);
            b.getInt();
            T.eq(5, b.getShort() & 0xFFFF, "segments sent");
            b.getShort();
            T.eq(250_000L, b.getLong(), "the stored bandwidth limit is sent with START");
            T.eq(1, b.get() & 0xFF, "checksum type sent");
            byte[] ck = new byte[16]; b.get(ck); b.position(b.position() + 16 + 7);
            T.eq("0123456789abcdef0123456789abcdef", java.util.HexFormat.of().formatHex(ck), "the checksum is sent as bytes");
            int ul = b.getShort() & 0xFFFF; byte[] ub = new byte[ul]; b.get(ub);
            T.eq("http://127.0.0.1:9/a.bin", new String(ub, StandardCharsets.UTF_8), "URL sent");
            T.within("RUNNING was announced to the UI", 2000, () -> e.ui.stateHistory(a.getId()).contains(STATUS_RUNNING));

            Download b2 = e.start("b.bin");
            Download c3 = e.start("c.bin");
            Download d4 = e.start("d.bin");
            T.eq(STATUS_RUNNING, b2.getStatus(), "second download runs");
            T.eq(STATUS_PENDING, c3.getStatus(), "third waits (limit 2)");
            T.eq(STATUS_PENDING, d4.getStatus(), "fourth waits");
            T.eq(2, e.core.commands(FakeCore.START).size(), "only two STARTs reached the core");
            T.ok(e.core.commands(FakeCore.START, c3.getId()).isEmpty(), "the waiting ones were never sent");
            T.eq(2, e.queue.activeCount(), "two slots in use");
            T.eq(List.of(c3.getId(), d4.getId()), e.queue.waitingIds(), "waiting in arrival order");
            var stats = e.svc.getStats();
            T.ok(stats.getActive() == 2 && stats.getQueued() == 2 && stats.getTotal() == 4, "stats: 2 active, 2 queued, 4 total");

            e.core.emitComplete(a.getId(), 1_000_000);
            T.within("when #1 completes, #3 is started (FIFO)", 4000, () -> e.statusIs(c3.getId(), STATUS_RUNNING));
            T.eq(STATUS_DONE, e.status(a.getId()), "#1 is DONE");
            T.eq(1, e.core.commands(FakeCore.START, c3.getId()).size(), "exactly one START for #3");
            T.eq(STATUS_PENDING, e.status(d4.getId()), "#4 still waits");
            T.eq(1_000_000L, e.repo.findById(a.getId()).get().getTotalSize(), "total size recorded");
            T.eq(1, e.ui.of("complete", a.getId()).size(), "completion announced once");
        }
    }

    static void priorityOrder() throws Exception {
        T.section("Queue order follows priority");
        try (Env e = new Env(1)) {
            Download run = e.start("run.bin");
            Download low = e.start("low.bin", 2);
            Download mid = e.start("mid.bin", 5);
            Download hi  = e.start("hi.bin", 9);
            Download mid2 = e.start("mid2.bin", 5);
            T.eq(List.of(hi.getId(), mid.getId(), mid2.getId(), low.getId()), e.queue.waitingIds(), "line: 9, then 5 (arrival order), then 2");
            e.core.emitComplete(run.getId(), 10);
            T.within("the priority-9 download is next", 4000, () -> e.statusIs(hi.getId(), STATUS_RUNNING));
            T.eq(STATUS_PENDING, e.status(mid.getId()), "the others keep waiting");
            e.core.emitComplete(hi.getId(), 10);
            T.within("then the first priority-5 download", 4000, () -> e.statusIs(mid.getId(), STATUS_RUNNING));
        }
    }

    static void slotsAreReleasedOnce() throws Exception {
        T.section("A slot is released once, however many signals say the download ended");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            Download c = e.start("c.bin");
            e.core.emitError(a.getId(), 2, "connection reset by peer");
            T.within("an error frees A's slot: B starts", 4000, () -> e.statusIs(b.getId(), STATUS_RUNNING));
            e.core.emitError(a.getId(), 2, "connection reset by peer");      /* the same failure reported again */
            e.svc.cancel(a.getId());                                          /* and the user removes it too */
            T.sleep(300);
            T.eq(STATUS_RUNNING, e.status(b.getId()), "B still runs");
            T.eq(STATUS_PENDING, e.status(c.getId()), "C still waits: the slot was not released a second time");
            T.eq(0, e.core.commands(FakeCore.START, c.getId()).size(), "no START for C");
            T.eq(1, e.queue.activeCount(), "one slot in use");
            T.ok(!e.repo.findById(a.getId()).isPresent(), "A was removed");

            e.core.emitComplete(b.getId(), 10);
            e.core.emitComplete(b.getId(), 10);                               /* duplicate completion */
            T.within("completing B starts C", 4000, () -> e.statusIs(c.getId(), STATUS_RUNNING));
            T.sleep(200);
            T.eq(1, e.core.commands(FakeCore.START, c.getId()).size(), "...once");
            T.eq(1, e.ui.of("complete", b.getId()).size(), "the duplicate completion was not announced again");
            T.eq(1, e.queue.activeCount(), "slot count correct");
        }
    }

    static void pausing() throws Exception {
        T.section("Pause");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            Download c = e.start("c.bin");

            Download p = e.svc.pause(a.getId());
            T.eq(STATUS_PAUSED, p.getStatus(), "pause() returns the download PAUSED");
            T.eq(FakeCore.PAUSED, e.core.dl(a.getId()).state, "the core paused it");
            T.within("pausing frees the slot: B starts", 4000, () -> e.statusIs(b.getId(), STATUS_RUNNING));
            T.eq(STATUS_PENDING, e.status(c.getId()), "C waits");
            T.eq(1, e.core.commands(FakeCore.PAUSE, a.getId()).size(), "one PAUSE sent");

            T.eq(STATUS_PAUSED, e.svc.pause(a.getId()).getStatus(), "pausing again is a no-op that still answers PAUSED");
            T.eq(1, e.core.commands(FakeCore.PAUSE, a.getId()).size(), "...without another PAUSE reaching the core");

            Download pc = e.svc.pause(c.getId());
            T.eq(STATUS_PAUSED, pc.getStatus(), "a WAITING download can be paused");
            T.eq(0, e.core.commands(FakeCore.PAUSE, c.getId()).size(), "...without involving the core (it never saw it)");
            T.ok(!e.queue.isWaiting(c.getId()), "...and it left the line");

            e.core.emitComplete(b.getId(), 10);
            T.within("B completes", 4000, () -> e.statusIs(b.getId(), STATUS_DONE));
            T.sleep(200);
            T.eq(STATUS_PAUSED, e.status(c.getId()), "the paused C is not started when a slot frees up");
            T.eq(0, e.queue.activeCount(), "no slot is held");

            T.eq(Kind.CONFLICT, kindOf(() -> e.svc.pause(b.getId())), "pausing a finished download is a conflict (409)");
            T.eq(Kind.NOT_FOUND, kindOf(() -> e.svc.pause(4040)), "pausing an unknown id is 404");
        }
    }

    static void resuming() throws Exception {
        T.section("Resume");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            e.svc.pause(a.getId());
            T.within("B takes the freed slot", 4000, () -> e.statusIs(b.getId(), STATUS_RUNNING));

            Download r = e.svc.resume(a.getId());
            T.eq(STATUS_PENDING, r.getStatus(), "resume() while every slot is busy leaves the download waiting (PENDING)");
            T.eq(0, e.core.commands(FakeCore.RESUME, a.getId()).size(), "...no RESUME sent yet");
            e.core.emitComplete(b.getId(), 10);
            T.within("when B finishes, A is resumed", 4000, () -> e.statusIs(a.getId(), STATUS_RUNNING));
            T.eq(1, e.core.commands(FakeCore.RESUME, a.getId()).size(), "with a RESUME (not a second START)");
            T.eq(1, e.core.commands(FakeCore.START, a.getId()).size(), "...A's START count is still 1");
            T.eq(STATUS_RUNNING, e.svc.resume(a.getId()).getStatus(), "resuming a running download is a no-op");
            T.eq(1, e.core.commands(FakeCore.RESUME, a.getId()).size(), "...no extra RESUME");
            T.eq(Kind.CONFLICT, kindOf(() -> e.svc.resume(b.getId())), "resuming a finished download is a conflict");

            /* The core has forgotten the download: RESUME -> NOT_FOUND -> START instead. */
            e.svc.pause(a.getId());
            e.core.forget(a.getId());
            Download again = e.svc.resume(a.getId());
            T.eq(STATUS_RUNNING, again.getStatus(), "resume works even if the core forgot the download");
            T.eq(1, e.core.commands(FakeCore.RESUME, a.getId()).size() - 1, "a RESUME was tried (NOT_FOUND)...");
            T.eq(2, e.core.commands(FakeCore.START, a.getId()).size(), "...and then a START carried it on");

            /* Resume after a failure. */
            e.core.emitError(a.getId(), 2, "boom");
            T.within("failed", 4000, () -> e.statusIs(a.getId(), STATUS_ERROR));
            Download retried = e.svc.resume(a.getId());
            T.eq(STATUS_RUNNING, retried.getStatus(), "a failed download can be retried");
            T.eq(null, retried.getErrorMessage(), "the old error text is cleared");
        }
    }

    static void cancelling() throws Exception {
        T.section("Cancel");
        Path dir = Files.createTempDirectory("fxsvc");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            Download c = e.start("c.bin");

            e.svc.cancel(c.getId());
            T.ok(e.repo.findById(c.getId()).isEmpty(), "a waiting download is removed");
            T.eq(0, e.core.commands(FakeCore.CANCEL, c.getId()).size(), "...without telling the core (it never knew it)");
            T.eq(1, e.queue.activeCount(), "slot count unchanged");
            T.eq(List.of(b.getId()), e.queue.waitingIds(), "B still waits");

            e.svc.cancel(a.getId());
            T.ok(e.repo.findById(a.getId()).isEmpty(), "a running download is removed");
            T.eq(1, e.core.commands(FakeCore.CANCEL, a.getId()).size(), "...and the core was told to cancel it");
            T.ok(e.core.dl(a.getId()) == null, "the core forgot it");
            T.within("its slot goes to B", 4000, () -> e.statusIs(b.getId(), STATUS_RUNNING));
            T.eq(1, e.ui.of("cancelled", a.getId()).size(), "cancellation announced");

            e.svc.cancel(777);                                  /* no such download: not an error */
            T.ok(true, "cancelling an unknown id does not throw");

            /* A finished download: the row goes, the file stays. */
            Path done = dir.resolve("done.bin");
            Files.writeString(done, "finished content");
            Download dd = row(0, "x", STATUS_DONE, 5);
            dd.setId(null);
            dd.setFilePath(done.toString());
            Download saved = e.repo.insert(dd);
            e.svc.cancel(saved.getId());
            T.ok(e.repo.findById(saved.getId()).isEmpty(), "a finished download's row is removed");
            T.ok(Files.exists(done), "...and its file is NEVER touched");
            T.eq(0, e.core.commands(FakeCore.CANCEL, saved.getId()).size(), "...the core is not involved");

            /* A paused download the core no longer knows about: leftovers are cleaned up here — but only
             * if the core's .fluxpart checkpoint proves it is a partial download. */
            Path part = dir.resolve("part.bin"), sidecar = dir.resolve("part.bin.fluxpart");
            Files.writeString(part, "partial"); Files.writeString(sidecar, "state");
            Download pd = row(0, "x", STATUS_PAUSED, 5); pd.setId(null); pd.setFilePath(part.toString());
            Download ps = e.repo.insert(pd);
            e.svc.cancel(ps.getId());
            T.ok(!Files.exists(part) && !Files.exists(sidecar), "a paused partial download's data and checkpoint are removed");

            Path user = dir.resolve("user.bin");
            Files.writeString(user, "not ours");
            Download ud = row(0, "x", STATUS_PAUSED, 5); ud.setId(null); ud.setFilePath(user.toString());
            Download us = e.repo.insert(ud);
            e.svc.cancel(us.getId());
            T.ok(Files.exists(user), "a file WITHOUT a checkpoint is never deleted");

            /* The core is unreachable: cancelling something it holds must not pretend to have worked. */
            Download live = e.svc.pause(b.getId());
            e.core.kill();
            T.within("daemon notices the core is gone", 3000, () -> !e.svc.isSynced());
            T.eq(Kind.UNAVAILABLE, kindOf(() -> e.svc.cancel(live.getId())), "cancel needs the core: 503, and the row is kept");
            T.ok(e.repo.findById(live.getId()).isPresent(), "the row is still there");
        } finally {
            try (var s = Files.walk(dir)) { s.sorted(java.util.Comparator.reverseOrder()).forEach(p -> p.toFile().delete()); }
        }
    }

    static void asyncErrorsAndCompletion() throws Exception {
        T.section("Errors and completion pushed by the core");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            e.core.emitError(a.getId(), CoreIPCClient.ERR_CHECKSUM, "checksum mismatch: expected 0123..., got 4567... (file removed)");
            T.within("an error marks the download ERROR", 4000, () -> e.statusIs(a.getId(), STATUS_ERROR));
            T.eq("checksum mismatch: expected 0123..., got 4567... (file removed)", e.repo.findById(a.getId()).get().getErrorMessage(), "with the core's own text");
            T.within("...and passes the slot on", 4000, () -> e.statusIs(b.getId(), STATUS_RUNNING));
            T.eq(1, e.ui.of("error", a.getId()).size(), "the UI was told once");
            T.ok(e.ui.stateHistory(a.getId()).contains(STATUS_ERROR), "...with a state change as well");

            e.svc.pause(b.getId());
            e.core.emitError(b.getId(), 2, "late failure of a paused download");
            T.sleep(250);
            T.eq(STATUS_PAUSED, e.status(b.getId()), "a late error does not undo a pause");

            e.core.emitError(31337, 2, "who?");
            T.sleep(100);
            T.ok(true, "an error for an unknown download is ignored");

            /* A download that finishes while the user is pausing it is simply done. */
            Download c = e.start("c.bin");
            e.core.emitComplete(c.getId(), 55);
            T.within("completion", 4000, () -> e.statusIs(c.getId(), STATUS_DONE));
            T.eq(Kind.CONFLICT, kindOf(() -> e.svc.pause(c.getId())), "pausing it now is a conflict");
            e.core.emitComplete(b.getId(), 42);
            T.within("a PAUSED download that completes anyway becomes DONE", 4000, () -> e.statusIs(b.getId(), STATUS_DONE));
        }
    }

    static void syncStartRejection() throws Exception {
        T.section("The core refuses a START");
        try (Env e = new Env(1)) {
            FakeCore.Dl squatter = e.core.put(900, FakeCore.DOWNLOADING, 10, 0);
            squatter.path = "/tmp/fxsvc/taken.bin";
            Download a = e.svc.start(req("taken.bin"));
            T.eq(STATUS_ERROR, a.getStatus(), "the download ends up ERROR");
            T.ok(a.getErrorMessage() != null && a.getErrorMessage().contains("already used"), "with the core's explanation (" + a.getErrorMessage() + ")");
            T.eq(0, e.queue.activeCount(), "its slot was given back");
            Download b = e.start("ok.bin");
            T.eq(STATUS_RUNNING, b.getStatus(), "the next download starts normally");
        }
    }

    static void progressHandling() throws Exception {
        T.section("Progress");
        try (Env e = new Env(2)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            e.core.emitProgress(a.getId(), 100, 1000, 5000);
            T.within("progress is broadcast", 3000, () -> !e.ui.of("progress", a.getId()).isEmpty());
            var m = e.ui.of("progress", a.getId()).get(0);
            T.ok(((Number) m.get("downloadedBytes")).longValue() == 100 && ((Number) m.get("speedBps")).longValue() == 5000
                 && ((Number) m.get("totalBytes")).longValue() == 1000 && ((Number) m.get("status")).intValue() == FakeCore.DOWNLOADING
                 && m.get("etaSeconds") != null, "broadcast carries totals, speed, status and eta");
            T.within("the first progress is stored at once (total size known early)", 3000,
                     () -> e.repo.findById(a.getId()).get().getDownloaded() == 100 && e.repo.findById(a.getId()).get().getTotalSize() == 1000);
            T.eq(5000L, e.svc.findById(a.getId()).get().getSpeedBps(), "the live speed is part of the download the API returns");
            T.eq(5000L, e.svc.getStats().getSpeedBps(), "...and of the stats");

            int savesBefore = e.repo.saves.get();
            for (int i = 1; i <= 200; i++) e.core.emitProgress(a.getId(), 100 + i, 1000, 5000 + i);
            T.within("the latest value is broadcast", 3000, () -> e.svc.findById(a.getId()).get().getSpeedBps() == 5200);
            T.ok(e.repo.saves.get() - savesBefore <= 3, "200 progress events cause only " + (e.repo.saves.get() - savesBefore) + " database write(s)");

            e.svc.pause(a.getId());
            int n = e.ui.of("progress", a.getId()).size();
            long before = e.repo.findById(a.getId()).get().getDownloaded();
            e.core.emitProgress(a.getId(), 999, 1000, 1);
            T.sleep(250);
            T.eq(n, e.ui.of("progress", a.getId()).size(), "a late progress event for a paused download is ignored");
            T.eq(before, e.repo.findById(a.getId()).get().getDownloaded(), "...and not stored");
            T.eq(0L, e.svc.findById(a.getId()).get().getSpeedBps(), "a paused download shows no speed");

            T.eq(5, e.ui.of("state", -1).size() >= 1 ? 5 : 0, "state changes were pushed to the UI");
        }
    }

    static void coreUnreachable() throws Exception {
        T.section("While the core is unreachable");
        try (Env e = new Env(2)) {
            Download a = e.start("a.bin");
            Download parked = e.repo.insert(row(0, "parked.bin", STATUS_PAUSED, 5));
            Download done = e.repo.insert(row(0, "done.bin", STATUS_DONE, 5));
            e.core.kill();
            T.within("the daemon notices", 3000, () -> !e.svc.isSynced());
            T.ok(e.ui.all.stream().anyMatch(m -> m.destination().equals("/topic/core") && "down".equals(m.get("state"))), "the UI is told the core is down");
            T.eq(Kind.UNAVAILABLE, kindOf(() -> e.svc.start(req("b.bin"))), "start: 503");
            T.eq(Kind.UNAVAILABLE, kindOf(() -> e.svc.pause(a.getId())), "pause: 503");
            T.eq(null, kindOf(() -> e.svc.resume(a.getId())), "resume of a download that is running is a no-op and needs no core");
            T.eq(Kind.UNAVAILABLE, kindOf(() -> e.svc.resume(parked.getId())), "resume of a paused one needs the core: 503");
            T.eq(STATUS_PAUSED, e.status(parked.getId()), "...and it stays paused (nothing was half-done)");
            T.eq(3, e.svc.getAll().size(), "reading the list still works");
            e.svc.cancel(done.getId());
            T.ok(e.repo.findById(done.getId()).isEmpty(), "removing a finished download needs no core");
            T.eq(STATUS_RUNNING, e.status(a.getId()), "the running download is NOT marked failed just because the core is away");
        }
    }

    static void reconcileAfterRestart() throws Exception {
        T.section("Reconciliation: daemon starts, the core is a fresh process");
        try (Env e = new Env(2, true, env -> {
                env.repo.insert(row(0, "r1.bin", STATUS_RUNNING, 5));    /* id 1 */
                env.repo.insert(row(0, "r2.bin", STATUS_RUNNING, 9));    /* id 2: higher priority */
                env.repo.insert(row(0, "p3.bin", STATUS_PENDING, 5));    /* id 3: was waiting */
                env.repo.insert(row(0, "z4.bin", STATUS_PAUSED, 5));     /* id 4 */
                env.repo.insert(row(0, "d5.bin", STATUS_DONE, 5));       /* id 5 */
                env.repo.insert(row(0, "e6.bin", STATUS_ERROR, 5));      /* id 6 */
            }, null, true)) {
            T.within("the two highest-priority unfinished downloads are started again", 4000, () -> e.core.commands(FakeCore.START).size() == 2);
            List<Long> started = new ArrayList<>();
            for (FakeCore.Cmd c : e.core.commands(FakeCore.START)) started.add(c.id());
            T.eq(List.of(2L, 1L), started, "in priority order: 2 (priority 9), then 1");
            T.within("both are RUNNING again", 4000, () -> e.statusIs(1, STATUS_RUNNING) && e.statusIs(2, STATUS_RUNNING));
            T.eq(STATUS_PENDING, e.status(3), "the third stays PENDING and waits for a slot (limit 2)");
            T.eq(STATUS_PAUSED, e.status(4), "a paused download stays paused");
            T.eq(STATUS_DONE, e.status(5), "a finished one stays finished");
            T.eq(STATUS_ERROR, e.status(6), "a failed one stays failed");
            T.eq(2, e.queue.activeCount(), "two slots in use");
            T.eq(List.of(3L), e.queue.waitingIds(), "#3 is waiting");
            e.core.emitComplete(2, 100);
            T.within("when one finishes the waiting one starts", 4000, () -> e.statusIs(3, STATUS_RUNNING));
            T.eq(1, e.core.commands(FakeCore.START, 3).size(), "with a START (the core never knew it)");
        }
    }

    static void reconcileAdoptsRunningWork() throws Exception {
        T.section("Reconciliation: the daemon restarted but the core kept working");
        try (Env e = new Env(2, true, env -> {
                env.repo.insert(row(0, "a.bin", STATUS_RUNNING, 5));    /* id 1: still running in the core */
                env.repo.insert(row(0, "b.bin", STATUS_PENDING, 5));    /* id 2: was waiting */
                env.repo.insert(row(0, "c.bin", STATUS_PENDING, 5));    /* id 3: was waiting */
            }, core -> core.put(1, FakeCore.DOWNLOADING, 1000, 400), true)) {
            T.eq(0, e.core.commands(FakeCore.START, 1).size(), "the running download is NOT started a second time");
            T.eq(STATUS_RUNNING, e.status(1), "it is RUNNING");
            T.eq(400L, e.repo.findById(1L).get().getDownloaded(), "its progress was taken from the core");
            T.eq(1000L, e.repo.findById(1L).get().getTotalSize(), "...and its size");
            T.within("one free slot left: #2 starts", 4000, () -> e.core.commands(FakeCore.START, 2).size() == 1);
            T.eq(STATUS_PENDING, e.status(3), "#3 waits: the adopted download holds a slot");
            T.ok(e.queue.isActive(1) && e.queue.isActive(2) && e.queue.isWaiting(3), "slot books: 1 and 2 active, 3 waiting");

            e.core.emitProgress(1, 500, 1000, 7777);
            T.within("events for the adopted download are handled normally", 3000, () -> e.svc.findById(1).get().getSpeedBps() == 7777);
            e.core.emitComplete(1, 1000);
            T.within("and it completes", 4000, () -> e.statusIs(1, STATUS_DONE));
            T.within("freeing its slot starts #3", 4000, () -> e.core.commands(FakeCore.START, 3).size() == 1);
        }
    }

    static void reconcileCorrections() throws Exception {
        T.section("Reconciliation: what happened while the daemon was away");
        try (Env e = new Env(3, true, env -> {
                env.repo.insert(row(0, "done.bin", STATUS_RUNNING, 5));   /* id 1: core finished it */
                env.repo.insert(row(0, "bad.bin",  STATUS_RUNNING, 5));   /* id 2: core failed it */
                env.repo.insert(row(0, "paus.bin", STATUS_RUNNING, 5));   /* id 3: core has it paused */
                env.repo.insert(row(0, "err.bin",  STATUS_ERROR,   5));   /* id 4: we gave up, the core is working on it */
            }, core -> {
                core.put(1, FakeCore.COMPLETE, 2000, 2000);
                core.put(2, FakeCore.ERROR, 2000, 10);
                core.put(3, FakeCore.PAUSED, 3000, 1000);
                core.put(4, FakeCore.DOWNLOADING, 4000, 3000);
                core.put(55, FakeCore.DOWNLOADING, 10, 1);            /* not in the database at all */
            }, true)) {
            T.eq(STATUS_DONE, e.status(1), "completed in the core -> DONE");
            T.eq(2000L, e.repo.findById(1L).get().getDownloaded(), "...with its size");
            T.eq(STATUS_ERROR, e.status(2), "failed in the core -> ERROR");
            T.ok(e.repo.findById(2L).get().getErrorMessage() != null, "...with an explanation");
            T.eq(STATUS_PAUSED, e.status(3), "paused in the core -> PAUSED");
            T.eq(STATUS_RUNNING, e.status(4), "a download we had written off but the core is running is RUNNING again");
            T.eq(0, e.core.commands(FakeCore.START).size(), "nothing was started: the core already had all of it");
            T.ok(e.queue.isActive(4) && !e.queue.isActive(1) && !e.queue.isActive(2) && !e.queue.isActive(3), "only the running one holds a slot");
            T.ok(e.core.dl(55) != null && e.core.commands(FakeCore.CANCEL).isEmpty(), "work for ids the daemon does not know is left alone, not cancelled");
        }

        try (Env e = new Env(2, false, env -> {
                env.repo.insert(row(0, "a.bin", STATUS_RUNNING, 5));
                env.repo.insert(row(0, "b.bin", STATUS_PENDING, 5));
            }, null, true)) {
            T.eq(STATUS_PAUSED, e.status(1), "auto-resume off: a download that was running comes back PAUSED");
            T.eq(STATUS_PAUSED, e.status(2), "...and so does one that was waiting");
            T.eq(0, e.core.commands(FakeCore.START).size(), "nothing is started on its own");
            Download r = e.svc.resume(1);
            T.eq(STATUS_RUNNING, r.getStatus(), "the user can resume it (RESUME -> NOT_FOUND -> START)");
        }
    }

    static void notSyncedUntilReconciled() throws Exception {
        T.section("Operations wait for reconciliation");
        try (Env e = new Env(2, true, env -> env.repo.insert(row(0, "a.bin", STATUS_RUNNING, 5)), core -> core.replyDelay(400), false)) {
            T.ok(!e.svc.isSynced(), "right after connecting the daemon is not yet in step");
            T.eq(Kind.UNAVAILABLE, kindOf(() -> e.svc.start(req("n.bin"))), "so start() answers 503 instead of guessing");
            T.within("then reconciliation finishes", 5000, e.svc::isSynced);
            e.core.replyDelay(0);
            T.eq(STATUS_RUNNING, e.svc.start(req("n.bin")).getStatus(), "and start() works");
        }
    }

    static void reconnectAfterCoreRestart() throws Exception {
        T.section("The core crashes and comes back empty");
        try (Env e = new Env(2)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");
            Download c = e.start("c.bin");                       /* waits */
            e.core.emitProgress(a.getId(), 5, 10, 1);
            T.eq(2, e.core.commands(FakeCore.START).size(), "before the crash: one START each for A and B");
            e.core.restart();                                     /* connection drops, new empty core */
            T.within("the loss is noticed", 3000, () -> !e.ipc.isConnected());
            e.ipc.connect();                                      /* what the supervisor does */
            T.within("reconciled again", 5000, e.svc::isSynced);
            T.within("both previously running downloads are started again (second START each)", 4000,
                     () -> e.core.commands(FakeCore.START, a.getId()).size() == 2 && e.core.commands(FakeCore.START, b.getId()).size() == 2);
            T.eq(0, e.core.commands(FakeCore.START, c.getId()).size(), "the waiting one was never sent");
            T.eq(STATUS_PENDING, e.status(c.getId()), "the waiting one is still waiting");
            T.within("A is RUNNING", 3000, () -> e.statusIs(a.getId(), STATUS_RUNNING));
            T.ok(e.ui.all.stream().anyMatch(m -> m.destination().equals("/topic/core") && "up".equals(m.get("state"))), "the UI is told the core is back");
            e.core.emitComplete(a.getId(), 10);
            T.within("the books are right: completing A starts C", 4000, () -> e.statusIs(c.getId(), STATUS_RUNNING));
        }
    }

    static void limits() throws Exception {
        T.section("Per-download speed limit");
        try (Env e = new Env(2)) {
            Download a = e.start("a.bin");
            Download r = e.svc.setLimit(a.getId(), 123_456);
            T.eq(123_456L, r.getBandwidthLimit(), "the limit is stored");
            T.eq(123_456L, e.core.dl(a.getId()).limit, "and applied in the core");
            e.svc.pause(a.getId());
            e.core.forget(a.getId());
            Download paused = e.repo.findById(a.getId()).get();
            Download w = e.svc.setLimit(paused.getId(), 99);
            T.eq(99L, w.getBandwidthLimit(), "a limit can be set while the core does not know the download (NOT_FOUND is fine)");
            e.svc.resume(a.getId());
            FakeCore.Cmd start = e.core.commands(FakeCore.START, a.getId()).get(1);
            T.eq(99L, ByteBuffer.wrap(start.payload()).order(ByteOrder.LITTLE_ENDIAN).getLong(8), "the stored limit is used the next time it is started");
            T.eq(Kind.BAD_REQUEST, kindOf(() -> e.svc.setLimit(a.getId(), -5)), "a negative limit is refused");
            T.eq(Kind.NOT_FOUND, kindOf(() -> e.svc.setLimit(9999, 5)), "an unknown download is 404");
        }
    }

    static void commandsDuringLaunch() throws Exception {
        T.section("A user command on a download whose START is still in flight waits for it");
        try (Env e = new Env(1)) {
            Download a = e.start("a.bin");
            Download b = e.start("b.bin");                      /* waiting */
            e.core.replyDelay(300);
            e.core.emitComplete(a.getId(), 10);                 /* B is promoted: its START goes out */
            T.within("B's START reached the core (reply still pending)", 3000, () -> e.core.commands(FakeCore.START, b.getId()).size() == 1);
            long t0 = System.nanoTime();
            Download p = e.svc.pause(b.getId());                /* the user pauses it right now */
            long ms = (System.nanoTime() - t0) / 1_000_000;
            e.core.replyDelay(0);
            T.eq(STATUS_PAUSED, p.getStatus(), "the pause wins");
            T.ok(ms >= 250, "pause() waited for the START to settle first (" + ms + " ms)");
            int startIdx = -1, pauseIdx = -1, i = 0;
            for (FakeCore.Cmd c : e.core.commands) {
                if (c.type() == FakeCore.START && c.id() == b.getId()) startIdx = i;
                if (c.type() == FakeCore.PAUSE && c.id() == b.getId()) pauseIdx = i;
                i++;
            }
            T.ok(startIdx >= 0 && pauseIdx > startIdx, "the core saw START before PAUSE");
            T.eq(FakeCore.PAUSED, e.core.dl(b.getId()).state, "so it ended up paused in the core too (no zombie)");
            T.eq(0, e.queue.activeCount(), "no slot is held");

            /* Same with cancel. */
            Download c = e.start("c.bin");
            Download d = e.start("d.bin");                      /* waiting */
            e.core.replyDelay(300);
            e.core.emitComplete(c.getId(), 10);
            T.within("D's START reached the core", 3000, () -> e.core.commands(FakeCore.START, d.getId()).size() == 1);
            e.svc.cancel(d.getId());
            e.core.replyDelay(0);
            T.ok(e.core.dl(d.getId()) == null, "cancel waited for the START and then removed it from the core");
            T.ok(e.repo.findById(d.getId()).isEmpty(), "and from the database");
        }
    }

    /** START's OK can reach the daemon after the download has already been paused, deleted or marked failed
     *  by something else. The core has accepted it by then, so the daemon must undo that instead of
     *  leaving a download running that the books say is not. */
    static void lateAcknowledgement() throws Exception {
        T.section("The core's OK arrives after the download changed under it");
        try (Env e = new Env(3)) {
            /* a) paused in the meantime */
            e.core.replyDelay(500);
            CompletableFuture<Download> f1 = CompletableFuture.supplyAsync(() -> e.start("late1.bin"));
            T.within("START reached the core (its reply is held back)", 3000, () -> e.core.commands(FakeCore.START).size() == 1);
            final Download r1 = e.repo.findAll().get(0);
            r1.setStatus(STATUS_PAUSED);
            e.repo.save(r1);
            f1.get(5, TimeUnit.SECONDS);
            e.core.replyDelay(0);
            T.within("the late OK is answered with a PAUSE", 4000, () -> e.core.commands(FakeCore.PAUSE, r1.getId()).size() == 1);
            T.within("so the core is not left running it", 3000, () -> e.core.dl(r1.getId()) != null && e.core.dl(r1.getId()).state == FakeCore.PAUSED);
            T.eq(STATUS_PAUSED, e.status(r1.getId()), "the row stays PAUSED: the late OK does not flip it to RUNNING");

            /* b) deleted in the meantime */
            e.core.replyDelay(500);
            CompletableFuture<Download> f2 = CompletableFuture.supplyAsync(() -> e.start("late2.bin"));
            T.within("the second START reached the core", 3000, () -> e.core.commands(FakeCore.START).size() == 2);
            final long id2 = e.repo.findAll().stream().filter(d -> d.getFilePath().endsWith("late2.bin")).findFirst().orElseThrow().getId();
            e.repo.deleteById(id2);
            f2.get(5, TimeUnit.SECONDS);
            e.core.replyDelay(0);
            T.within("the late OK is answered with a CANCEL", 4000, () -> e.core.commands(FakeCore.CANCEL, id2).size() == 1);
            T.within("so the core does not keep a download nobody knows about", 3000, () -> e.core.dl(id2) == null);

            /* c) written off as failed in the meantime */
            e.core.replyDelay(500);
            CompletableFuture<Download> f3 = CompletableFuture.supplyAsync(() -> e.start("late3.bin"));
            T.within("the third START reached the core", 3000, () -> e.core.commands(FakeCore.START).size() == 3);
            final Download r3 = e.repo.findAll().stream().filter(d -> d.getFilePath().endsWith("late3.bin")).findFirst().orElseThrow();
            r3.setStatus(STATUS_ERROR);
            r3.setErrorMessage("gave up");
            e.repo.save(r3);
            f3.get(5, TimeUnit.SECONDS);
            e.core.replyDelay(0);
            T.within("the late OK is answered with a CANCEL", 4000, () -> e.core.commands(FakeCore.CANCEL, r3.getId()).size() == 1);
            T.within("so a download we reported failed is not left running", 3000, () -> e.core.dl(r3.getId()) == null);
            T.eq(STATUS_ERROR, e.status(r3.getId()), "the row stays ERROR");
        }
    }

    static void timeoutThenAdopt() throws Exception {
        T.section("The core answers too late: nothing is lost, nothing runs twice");

        /* The usual case: the START was applied late, but the daemon's CANCEL followed it down the
         * same connection, so the core ends up with nothing and the database says so. */
        try (Env e = new Env(2)) {
            e.core.replyDelay(3000);                            /* longer than the 2 s reply timeout */
            Download a = e.svc.start(req("slow.bin"));
            T.eq(STATUS_ERROR, a.getStatus(), "start() reports the download as failed");
            T.ok(a.getErrorMessage() != null && a.getErrorMessage().contains("no reply"), "...saying why (" + a.getErrorMessage() + ")");
            T.within("the daemon dropped the unreliable connection", 3000, () -> !e.ipc.isConnected());
            e.core.replyDelay(0);
            T.within("the core got a CANCEL right behind the START", 6000, () -> e.core.commands(FakeCore.CANCEL, a.getId()).size() == 1);
            T.within("...so the download is not left running in the core", 3000, () -> e.core.dl(a.getId()) == null);
            T.eq(0, e.queue.activeCount(), "the slot was given back");
            e.ipc.connect();
            T.within("reconciled", 5000, e.svc::isSynced);
            T.eq(STATUS_ERROR, e.status(a.getId()), "the download stays ERROR: nothing is running in the core");
            T.eq(1, e.core.commands(FakeCore.START, a.getId()).size(), "no second START was sent behind the user's back");
            T.eq(0, e.queue.activeCount(), "no slot is held");
        }

        /* The rare case: the CANCEL did not get through either. The core is running a download the
         * daemon wrote off — when the daemon sees that, the core's word wins. */
        try (Env e = new Env(2)) {
            e.core.swallowCancel(true);
            e.core.replyDelay(3000);
            Download a = e.svc.start(req("slow2.bin"));
            T.eq(STATUS_ERROR, a.getStatus(), "start() reports the download as failed");
            T.within("the daemon dropped the unreliable connection", 3000, () -> !e.ipc.isConnected());
            e.core.replyDelay(0);
            T.within("the core did receive the START after all", 6000, () -> e.core.dl(a.getId()) != null);
            e.ipc.connect();
            T.within("reconciled", 5000, e.svc::isSynced);
            T.eq(STATUS_RUNNING, e.status(a.getId()), "the core's word wins: the download is RUNNING again");
            T.eq(null, e.repo.findById(a.getId()).get().getErrorMessage(), "...and the stale error text is gone");
            T.eq(1, e.core.commands(FakeCore.START, a.getId()).size(), "it was started once, not twice");
            T.ok(e.queue.isActive(a.getId()), "it holds a slot");
            e.core.emitComplete(a.getId(), 1000);
            T.within("and its completion is handled normally", 4000, () -> e.statusIs(a.getId(), STATUS_DONE));
            T.eq(0, e.queue.activeCount(), "slot given back");
        }
    }

    static void scheduler() throws Exception {
        T.section("Scheduled resume");
        try (Env e = new Env(2)) {
            Download a = e.start("a.bin");
            e.svc.pause(a.getId());
            MemSchedules schedules = new MemSchedules();
            SchedulerService sched = new SchedulerService(schedules, e.svc);

            ScheduleTask once = new ScheduleTask();
            once.setDownloadId(a.getId()); once.setTriggerAt(1L); once.setRepeatSecs(0L); once.setEnabled(true);
            ScheduleTask repeat = new ScheduleTask();
            repeat.setDownloadId(a.getId()); repeat.setTriggerAt(1L); repeat.setRepeatSecs(3600L); repeat.setEnabled(true);
            ScheduleTask future = new ScheduleTask();
            future.setDownloadId(a.getId()); future.setTriggerAt(Long.MAX_VALUE / 2); future.setRepeatSecs(0L); future.setEnabled(true);
            ScheduleTask gone = new ScheduleTask();
            gone.setDownloadId(31337L); gone.setTriggerAt(1L); gone.setRepeatSecs(0L); gone.setEnabled(true);
            schedules.save(once); schedules.save(repeat); schedules.save(future); schedules.save(gone);

            sched.tick();
            T.eq(STATUS_RUNNING, e.status(a.getId()), "a due task resumes the download");
            T.eq(false, once.getEnabled(), "a one-shot task is disabled after it fired");
            T.ok(repeat.getEnabled() && repeat.getTriggerAt() > System.currentTimeMillis() / 1000, "a repeating task is rescheduled");
            T.ok(future.getEnabled() && future.getTriggerAt() == Long.MAX_VALUE / 2, "a task that is not due is left alone");
            T.eq(false, gone.getEnabled(), "a task for a download that no longer exists does not break the tick (and is retired)");

            e.svc.pause(a.getId());
            e.core.kill();
            T.within("core down", 3000, () -> !e.svc.isSynced());
            ScheduleTask retry = new ScheduleTask();
            retry.setDownloadId(a.getId()); retry.setTriggerAt(1L); retry.setRepeatSecs(0L); retry.setEnabled(true);
            schedules.save(retry);
            sched.tick();
            T.eq(true, retry.getEnabled(), "if the core is unavailable the task is kept and tried again next tick");
        }
    }

    static void bandwidth() throws Exception {
        T.section("Global bandwidth limit");
        Path sock = Path.of("/tmp/fxb" + ProcessHandle.current().pid() + ".sock");
        try (FakeCore core = new FakeCore(sock)) {
            CoreIPCClient ipc = new CoreIPCClient(sock.toString());
            BandwidthService bw = new BandwidthService(ipc, 1234);
            ipc.addListener(bw);
            ipc.connect();
            T.within("the configured limit is applied when the core connects", 3000, () -> core.globalLimit() == 1234);
            bw.setGlobalLimit(5000);
            T.eq(5000L, core.globalLimit(), "setGlobalLimit reaches the core");
            T.eq(5000L, bw.getGlobalLimit(), "and is remembered");
            T.eq(Kind.BAD_REQUEST, kindOf(() -> bw.setGlobalLimit(-1)), "negative is refused");
            bw.setGlobalLimit(0);
            T.eq(0L, core.globalLimit(), "0 = unlimited is applied too");
            bw.setGlobalLimit(777);
            core.restart();                                      /* a fresh core knows nothing */
            T.within("connection lost", 3000, () -> !ipc.isConnected());
            ipc.connect();
            T.within("the user's latest limit is applied again after a restart", 3000, () -> core.globalLimit() == 777);
            core.kill();
            T.within("core gone", 3000, () -> !ipc.isConnected());
            bw.setGlobalLimit(888);
            T.eq(888L, bw.getGlobalLimit(), "a limit set while the core is away is remembered...");
            core.restart();
            ipc.connect();
            T.within("...and applied when it returns", 3000, () -> core.globalLimit() == 888);
            ipc.close();
        }
    }
}
