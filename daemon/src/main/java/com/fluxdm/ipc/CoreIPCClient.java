/* FluxDM — ipc/CoreIPCClient.java
 * Persistent Unix-domain-socket client for fluxdm-core.
 *
 * Wire format: ipc/protocol.h + ipc/messages.h (16-byte little-endian header,
 * then payload). What this class guarantees, and why:
 *
 *  - REPLIES vs EVENTS. The core marks frames it pushes on its own initiative
 *    (progress, completion, asynchronous errors) with FLUX_FLAG_EVENT. A reply
 *    to a command never carries it. Both an "async failure of download 7" and
 *    "the answer to my PAUSE 7" are RSP_ERROR frames with id 7; only the flag
 *    tells them apart, so it is the only thing this class trusts.
 *
 *  - REPLY MATCHING. The core answers commands in the order it read them, on
 *    one thread, per connection. Every command therefore joins one FIFO of
 *    pending commands — atomically with its bytes being written — and each
 *    reply frame completes the head of that FIFO. Nothing is keyed by download
 *    id, so two commands for the same download in flight at once (pause then
 *    resume from two REST threads) cannot steal each other's answers.
 *    A reply that does not fit the head of the FIFO means the two sides have
 *    lost step; the only safe response is to drop the connection and let the
 *    owner reconnect and resynchronise (IPCEventListener.onConnected).
 *
 *  - NOTHING HEAVY ON THE READER. The reader thread only parses frames. Every
 *    future completion and every listener callback runs on one dedicated
 *    dispatcher thread, in the order the core produced them, so a slow
 *    database write in a listener can delay later callbacks but can never stall
 *    the socket (the core disconnects clients that stop reading). Progress
 *    events are coalesced per download while waiting for that thread: only the
 *    newest is delivered. Completion/error events are never dropped.
 *
 *  - COMMANDS NEVER THROW. Every command returns a future that always
 *    completes with an AckResult — core errors carry the wire code (> 0),
 *    problems on this side carry a negative ERR_* code (not connected,
 *    connection lost, no reply within ACK_TIMEOUT_SECS, ...). Callers need one
 *    code path, not a try/catch plus a result check. A command that gets no
 *    reply in time also drops the connection (see submit()): after that the
 *    reply FIFO cannot be trusted any more.
 *
 *  - RECONNECTABLE. connect() may be called again after the connection was
 *    lost; every connection is a self-contained Link, so a late cleanup of an
 *    old link can never fail the commands of a new one.
 */
package com.fluxdm.ipc;

import com.fluxdm.security.RuntimeDir;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@Component
public class CoreIPCClient {

    private static final Logger log = LoggerFactory.getLogger(CoreIPCClient.class);

    /* ── Wire constants (must match ipc/protocol.h exactly) ─────────────── */
    public static final int  FLUX_MAGIC = 0x464C5558;
    public static final byte FLUX_VER   = 1;

    public static final byte CMD_START       = 0x01;
    public static final byte CMD_PAUSE       = 0x02;
    public static final byte CMD_RESUME      = 0x03;
    public static final byte CMD_CANCEL      = 0x04;
    public static final byte CMD_SET_LIMIT   = 0x05;
    public static final byte CMD_STATUS      = 0x06;
    public static final byte CMD_STATUS_ALL  = 0x07;
    public static final byte CMD_SHUTDOWN    = (byte) 0xFF;

    public static final byte RSP_OK          = 0x10;
    public static final byte RSP_ERROR       = 0x11;
    public static final byte RSP_PROGRESS    = 0x12;
    public static final byte RSP_COMPLETE    = 0x13;
    public static final byte RSP_STATUS      = 0x14;

    /** Header flag: the frame was pushed by the core, it is not a reply. */
    public static final int  FLAG_EVENT      = 0x0001;
    public static final int  MAX_PAYLOAD     = 1 << 20;

    /* Wire error codes sent by the core (FLUX_WIRE_ERR_* in ipc/protocol.h). */
    public static final int ERR_INVALID_URL = 1;
    public static final int ERR_NETWORK     = 2;
    public static final int ERR_TIMEOUT     = 3;
    public static final int ERR_DISK        = 4;
    public static final int ERR_TLS         = 5;
    public static final int ERR_NOT_FOUND   = 6;
    public static final int ERR_FORBIDDEN   = 7;
    public static final int ERR_SERVER      = 8;
    public static final int ERR_RANGE       = 9;
    public static final int ERR_CHECKSUM    = 10;
    public static final int ERR_INTERNAL    = 99;   /* includes "download is in the wrong state" */

    /* Problems on THIS side of the socket — never sent by the core, always negative. */
    public static final int ERR_NOT_CONNECTED   = -1;
    public static final int ERR_CONNECTION_LOST = -2;
    public static final int ERR_NO_REPLY        = -3;
    public static final int ERR_PROTOCOL        = -4;
    public static final int ERR_BAD_ARGUMENT    = -5;

    /* Download states reported by the core (flux_download_status_t). */
    public static final int CORE_IDLE        = 0;
    public static final int CORE_CONNECTING  = 1;
    public static final int CORE_DOWNLOADING = 2;
    public static final int CORE_PAUSED      = 3;
    public static final int CORE_COMPLETE    = 4;
    public static final int CORE_ERROR       = 5;
    public static final int CORE_CANCELLED   = 6;

    /** Wire value of "estimated time unknown" in progress/status frames. */
    public static final long ETA_UNKNOWN = 0xFFFFFFFFL;

    public static final int  ACK_TIMEOUT_SECS = 15;
    private static final int HEADER_SIZE      = 16;
    private static final int CMD_START_FIXED  = 56;   /* flux_cmd_start_t, without the strings */
    private static final int MAX_URL_BYTES    = 4095; /* core: FLUX_MAX_URL - 1  */
    private static final int MAX_PATH_BYTES   = 1023; /* core: FLUX_MAX_PATH - 1 */
    private static final int MAX_PENDING      = 1024; /* unanswered commands before we refuse more */

    /* ── State ────────────────────────────────────────────────────────────── */

    /** One command waiting for its reply. */
    private static final class Pending {
        enum Kind { SIMPLE, STATUS, STATUS_ALL }

        final Kind                           kind;
        final String                         what;
        final CompletableFuture<AckResult>   future = new CompletableFuture<>();
        final List<StatusInfo>               rows   = new ArrayList<>();   /* reader thread only */
        volatile ScheduledFuture<?>          timeout;

        Pending(Kind kind, String what) { this.kind = kind; this.what = what; }
    }

    /** One connection to the core, and everything that belongs to it alone. */
    private static final class Link {
        final SocketChannel      channel;
        final Object             writeLock = new Object();
        final ArrayDeque<Pending> pending  = new ArrayDeque<>();   /* guarded by itself */
        volatile boolean         closed;
        volatile boolean         deliberate;
        Thread                   reader;

        Link(SocketChannel channel) { this.channel = channel; }
    }

    private final String sockPath;
    private final AtomicReference<Link> link = new AtomicReference<>();
    private final Object lifecycle = new Object();    /* connect() vs addListener() */
    private volatile boolean closed;

    private final List<IPCEventListener> listeners = new CopyOnWriteArrayList<>();

    private volatile long ackTimeoutMs = ACK_TIMEOUT_SECS * 1000L;

    /* Newest undelivered progress per download (coalescing). */
    private final ConcurrentHashMap<Long, ProgressInfo> latestProgress = new ConcurrentHashMap<>();

    /* Dispatcher thread: completes futures, calls listeners, fires timeouts. */
    private final ScheduledThreadPoolExecutor events;

    /** An empty {@code sockPath} means the default: the private runtime directory (see {@link RuntimeDir}). */
    public CoreIPCClient(@Value("${fluxdm.core.socket-path:}") String sockPath) {
        this.sockPath = sockPath == null || sockPath.isBlank() ? defaultSocketPath() : sockPath;
        this.events = new ScheduledThreadPoolExecutor(1, r -> {
            Thread t = new Thread(r, "fluxdm-ipc-events");
            t.setDaemon(true);
            return t;
        });
        this.events.setRemoveOnCancelPolicy(true);
        this.events.setExecuteExistingDelayedTasksAfterShutdownPolicy(false);
    }

    public String socketPath() { return sockPath; }

    private static String defaultSocketPath() {
        try {
            return RuntimeDir.defaultSocket().toString();
        } catch (IOException e) {
            throw new IllegalStateException("cannot choose a socket location: " + e.getMessage(), e);
        }
    }

    /** How long a command may wait for the core's reply before its future completes with
     *  ERR_NO_REPLY. The default (ACK_TIMEOUT_SECS) is right for production; tests shorten it. */
    public void setAckTimeoutMillis(long ms) { this.ackTimeoutMs = Math.max(1L, ms); }

    /** Registers a listener. If the client is already connected the listener
     *  immediately receives onConnected(), so it never has to care whether it
     *  was created before or after the connection came up. */
    public void addListener(IPCEventListener l) {
        synchronized (lifecycle) {
            listeners.add(l);
            Link cur = link.get();
            if (cur != null && !cur.closed) dispatch(() -> safely(l::onConnected));
        }
    }

    public void removeListener(IPCEventListener l) { listeners.remove(l); }

    /* ── Lifecycle ────────────────────────────────────────────────────────── */

    /** Connects (or reconnects). Does nothing if already connected. */
    public void connect() throws IOException {
        synchronized (lifecycle) {
            if (closed) throw new IllegalStateException("CoreIPCClient is closed");
            Link cur = link.get();
            if (cur != null && !cur.closed) return;

            SocketChannel ch = SocketChannel.open(StandardProtocolFamily.UNIX);
            try {
                ch.connect(UnixDomainSocketAddress.of(Path.of(sockPath)));
            } catch (IOException | RuntimeException e) {
                try { ch.close(); } catch (IOException ignored) { /* nothing more to do */ }
                throw e;
            }

            Link l = new Link(ch);
            link.set(l);
            l.reader = new Thread(() -> readLoop(l), "fluxdm-ipc-reader");
            l.reader.setDaemon(true);
            l.reader.start();

            List<IPCEventListener> snapshot = new ArrayList<>(listeners);
            dispatch(() -> { for (IPCEventListener x : snapshot) safely(x::onConnected); });
        }
    }

    /** Drops the connection (pending commands fail with ERR_CONNECTION_LOST). */
    public void disconnect() {
        Link l = link.get();
        if (l != null) closeLink(l, "disconnected by the daemon", true);
    }

    public boolean isConnected() {
        Link l = link.get();
        return l != null && !l.closed && l.channel.isOpen();
    }

    /** Final shutdown: disconnects and stops the dispatcher thread. */
    @PreDestroy
    public void close() {
        synchronized (lifecycle) { closed = true; }
        disconnect();
        events.shutdown();
    }

    /* ── Commands ─────────────────────────────────────────────────────────── */

    public CompletableFuture<AckResult> start(long id, String url, String destPath,
                                               int segments, long bwLimitBps,
                                               int checksumType, byte[] checksum) {
        if (!validId(id, false)) return badId("START", id);
        if (url == null || destPath == null)
            return rejected("START needs a URL and a destination path");

        byte[] urlBytes  = url.getBytes(StandardCharsets.UTF_8);
        byte[] pathBytes = destPath.getBytes(StandardCharsets.UTF_8);
        if (urlBytes.length == 0 || urlBytes.length > MAX_URL_BYTES)
            return rejected("the URL must be 1.." + MAX_URL_BYTES + " bytes");
        if (pathBytes.length == 0 || pathBytes.length > MAX_PATH_BYTES)
            return rejected("the destination path must be 1.." + MAX_PATH_BYTES + " bytes");

        int payloadLen = CMD_START_FIXED + 2 + urlBytes.length + 2 + pathBytes.length;
        ByteBuffer buf = frame(CMD_START, id, payloadLen);
        buf.putInt((int) id);
        buf.putShort((short) Math.max(0, Math.min(segments, 0xFFFF)));
        buf.putShort((short) 0);                       /* flags, reserved */
        buf.putLong(Math.max(0L, bwLimitBps));
        buf.put((byte) checksumType);
        byte[] cksum32 = new byte[32];
        if (checksum != null) System.arraycopy(checksum, 0, cksum32, 0, Math.min(32, checksum.length));
        buf.put(cksum32);
        buf.put(new byte[7]);                          /* _pad */
        buf.putShort((short) urlBytes.length);
        buf.put(urlBytes);
        buf.putShort((short) pathBytes.length);
        buf.put(pathBytes);

        return submit(Pending.Kind.SIMPLE, "START " + id, buf.array());
    }

    public CompletableFuture<AckResult> pause(long id)  { return simple(CMD_PAUSE,  "PAUSE",  id); }
    public CompletableFuture<AckResult> resume(long id) { return simple(CMD_RESUME, "RESUME", id); }
    public CompletableFuture<AckResult> cancel(long id) { return simple(CMD_CANCEL, "CANCEL", id); }

    /** id = 0 sets the daemon-wide limit; any other id targets one download. 0 = unlimited. */
    public CompletableFuture<AckResult> setLimit(long id, long bytesPerSec) {
        if (!validId(id, true)) return badId("SET_LIMIT", id);
        ByteBuffer buf = frame(CMD_SET_LIMIT, id, 8);
        buf.putLong(Math.max(0L, bytesPerSec));
        return submit(Pending.Kind.SIMPLE, "SET_LIMIT " + id, buf.array());
    }

    /** The reply is also pushed to listeners (onStatus) and carried in AckResult.statuses(). */
    public CompletableFuture<AckResult> requestStatus(long id) {
        if (!validId(id, false)) return badId("STATUS", id);
        return submit(Pending.Kind.STATUS, "STATUS " + id, frame(CMD_STATUS, id, 0).array());
    }

    /** Every status row is pushed to listeners; the future completes, with all rows, after the last. */
    public CompletableFuture<AckResult> requestStatusAll() {
        return submit(Pending.Kind.STATUS_ALL, "STATUS_ALL", frame(CMD_STATUS_ALL, 0, 0).array());
    }

    public CompletableFuture<AckResult> shutdownCore() {
        return submit(Pending.Kind.SIMPLE, "SHUTDOWN", frame(CMD_SHUTDOWN, 0, 0).array());
    }

    private CompletableFuture<AckResult> simple(byte type, String name, long id) {
        if (!validId(id, false)) return badId(name, id);
        return submit(Pending.Kind.SIMPLE, name + " " + id, frame(type, id, 0).array());
    }

    private static boolean validId(long id, boolean allowZero) {
        return id <= 0xFFFFFFFFL && (allowZero ? id >= 0 : id >= 1);
    }

    private static CompletableFuture<AckResult> badId(String cmd, long id) {
        return rejected(cmd + ": invalid download id " + id);
    }

    private static CompletableFuture<AckResult> rejected(String why) {
        return CompletableFuture.completedFuture(AckResult.failure(ERR_BAD_ARGUMENT, why));
    }

    private static ByteBuffer frame(byte type, long id, int payloadLen) {
        ByteBuffer b = ByteBuffer.allocate(HEADER_SIZE + payloadLen).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(FLUX_MAGIC);
        b.put(FLUX_VER);
        b.put(type);
        b.putShort((short) 0);          /* flags: none for commands */
        b.putInt((int) id);             /* low 32 bits: ids are unsigned on the wire */
        b.putInt(payloadLen);
        return b;
    }

    /** Registers the command in the reply FIFO and writes it, atomically with
     *  respect to every other sender — the order of the FIFO is the order of
     *  the bytes on the socket. */
    private CompletableFuture<AckResult> submit(Pending.Kind kind, String what, byte[] bytes) {
        Pending p = new Pending(kind, what);
        Link l = link.get();
        if (l == null || l.closed) {
            p.future.complete(AckResult.failure(ERR_NOT_CONNECTED, "not connected to fluxdm-core"));
            return p.future;
        }

        synchronized (l.writeLock) {
            synchronized (l.pending) {
                if (l.closed) {
                    p.future.complete(AckResult.failure(ERR_NOT_CONNECTED, "not connected to fluxdm-core"));
                    return p.future;
                }
                if (l.pending.size() >= MAX_PENDING) {
                    p.future.complete(AckResult.failure(ERR_NO_REPLY,
                        "fluxdm-core has " + MAX_PENDING + " unanswered commands; refusing more"));
                    return p.future;
                }
                l.pending.addLast(p);
            }

            try {
                ByteBuffer bb = ByteBuffer.wrap(bytes);
                while (bb.hasRemaining()) l.channel.write(bb);
            } catch (IOException e) {
                /* A half-written frame would desynchronise the stream for good. */
                closeLink(l, "send failed: " + e.getMessage(), false);
                return p.future;      /* closeLink() failed it with everything else */
            }
        }

        /* A command whose reply never comes leaves the reply FIFO impossible to trust: the
         * core may answer late (the next reply then belongs to the NEXT command, as it
         * should) or never (every later reply would be matched to the wrong command).
         * There is no way to tell which, so the connection is dropped: the owner
         * reconnects and re-reads the core's state from scratch. */
        ScheduledFuture<?> t;
        long waitMs = ackTimeoutMs;
        try {
            t = events.schedule(() -> {
                if (p.future.complete(AckResult.failure(ERR_NO_REPLY,
                        "no reply from fluxdm-core to " + p.what + " within " + waitMs + " ms")))
                    closeLink(l, "no reply to " + p.what + " within " + waitMs + " ms", false);
            }, waitMs, TimeUnit.MILLISECONDS);
        } catch (RejectedExecutionException e) {
            return p.future;
        }
        p.timeout = t;
        if (p.future.isDone()) t.cancel(false);
        return p.future;
    }

    /* ── Reader thread ────────────────────────────────────────────────────── */

    private void readLoop(Link l) {
        String why = "fluxdm-core closed the connection";
        ByteBuffer hdr = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        try {
            while (!l.closed) {
                hdr.clear();
                if (!readFully(l.channel, hdr)) break;               /* EOF */
                hdr.flip();
                int  magic   = hdr.getInt();
                int  version = hdr.get() & 0xFF;
                int  type    = hdr.get() & 0xFF;
                int  flags   = hdr.getShort() & 0xFFFF;
                long id      = hdr.getInt() & 0xFFFFFFFFL;
                long plen    = hdr.getInt() & 0xFFFFFFFFL;

                if (magic != FLUX_MAGIC)  { why = String.format("bad frame magic 0x%08x", magic); break; }
                if (version != FLUX_VER)  { why = "unsupported protocol version " + version;       break; }
                if (plen > MAX_PAYLOAD)   { why = "oversized frame (" + plen + " bytes)";           break; }

                byte[] payload = new byte[(int) plen];
                if (plen > 0 && !readFully(l.channel, ByteBuffer.wrap(payload))) break;

                String problem = handleFrame(l, type, flags, id, payload);
                if (problem != null) { why = problem; break; }
            }
        } catch (IOException e) {
            if (!l.deliberate) why = "read error: " + e.getMessage();
        } catch (RuntimeException e) {
            why = "internal error in the reader: " + e;
            log.error("fluxdm-ipc reader failed", e);
        } finally {
            closeLink(l, why, l.deliberate);
        }
    }

    private static boolean readFully(SocketChannel ch, ByteBuffer buf) throws IOException {
        while (buf.hasRemaining()) {
            if (ch.read(buf) < 0) return false;
        }
        return true;
    }

    /** Returns null if the frame was handled, else why the connection must be dropped. */
    private String handleFrame(Link l, int type, int flags, long id, byte[] payload) {
        boolean event = (flags & FLAG_EVENT) != 0;

        switch ((byte) type) {
            case RSP_PROGRESS -> {
                ProgressInfo p = ProgressInfo.parse(payload);
                if (p == null) log.warn("ignoring a short RSP_PROGRESS frame ({} bytes)", payload.length);
                else offerProgress(p);
                return null;
            }
            case RSP_COMPLETE -> {
                CompleteInfo c = CompleteInfo.parse(payload);
                if (c == null) log.warn("ignoring a short RSP_COMPLETE frame ({} bytes)", payload.length);
                else dispatch(() -> notifyComplete(c));
                return null;
            }
            case RSP_STATUS -> {
                StatusInfo s = StatusInfo.parse(payload);
                if (event) {
                    if (s != null) dispatch(() -> notifyStatus(s));
                    return null;
                }
                if (s == null) return "malformed RSP_STATUS reply (" + payload.length + " bytes)";
                return replyStatus(l, s);
            }
            case RSP_OK -> {
                if (event) { log.warn("ignoring an RSP_OK event frame"); return null; }
                return replyOk(l);
            }
            case RSP_ERROR -> {
                int    code = payload.length > 0 ? (payload[0] & 0xFF) : 0;
                String msg  = payload.length > 4 ? cString(payload, 4, payload.length) : "";
                if (event) {
                    final long fid = id;
                    dispatch(() -> notifyError(fid, code, msg));
                    return null;
                }
                return replyError(l, code, msg);
            }
            default -> {
                log.warn("ignoring unknown frame type 0x{}", Integer.toHexString(type));
                return null;
            }
        }
    }

    private String replyOk(Link l) {
        Pending p;
        synchronized (l.pending) {
            p = l.pending.peekFirst();
            if (p == null) { log.warn("unsolicited RSP_OK from fluxdm-core ignored"); return null; }
            if (p.kind == Pending.Kind.STATUS)
                return "RSP_OK received where a status for " + p.what + " was expected";
            l.pending.pollFirst();
        }
        finish(p, p.kind == Pending.Kind.STATUS_ALL ? AckResult.success(List.copyOf(p.rows))
                                                    : AckResult.success());
        return null;
    }

    private String replyError(Link l, int code, String msg) {
        Pending p;
        synchronized (l.pending) {
            p = l.pending.pollFirst();
        }
        if (p == null) { log.warn("unsolicited RSP_ERROR reply from fluxdm-core ignored: {}", msg); return null; }
        finish(p, AckResult.failure(code, msg));
        return null;
    }

    private String replyStatus(Link l, StatusInfo s) {
        Pending p;
        synchronized (l.pending) {
            p = l.pending.peekFirst();
            if (p == null) { log.warn("unsolicited RSP_STATUS reply from fluxdm-core ignored"); return null; }
            if (p.kind == Pending.Kind.SIMPLE)
                return "RSP_STATUS received where a plain reply to " + p.what + " was expected";
            if (p.kind == Pending.Kind.STATUS) l.pending.pollFirst();
        }
        dispatch(() -> notifyStatus(s));
        if (p.kind == Pending.Kind.STATUS) finish(p, AckResult.success(List.of(s)));
        else                               p.rows.add(s);   /* STATUS_ALL: more rows, then RSP_OK */
        return null;
    }

    private void finish(Pending p, AckResult r) {
        ScheduledFuture<?> t = p.timeout;
        if (t != null) t.cancel(false);
        dispatch(() -> p.future.complete(r));
    }

    private void closeLink(Link l, String reason, boolean deliberate) {
        synchronized (l) {
            if (l.closed) return;
            l.closed = true;
            l.deliberate = deliberate;
        }
        link.compareAndSet(l, null);
        try { l.channel.close(); } catch (IOException ignored) { /* already gone */ }

        List<Pending> dead;
        synchronized (l.pending) {
            dead = new ArrayList<>(l.pending);
            l.pending.clear();
        }
        AckResult lost = AckResult.failure(ERR_CONNECTION_LOST, "connection to fluxdm-core lost: " + reason);
        for (Pending p : dead) finish(p, lost);

        if (!deliberate) {
            log.warn("lost the connection to fluxdm-core: {}", reason);
            List<IPCEventListener> snapshot = new ArrayList<>(listeners);
            dispatch(() -> { for (IPCEventListener x : snapshot) safely(() -> x.onConnectionLost(reason)); });
        }
    }

    /* ── Dispatcher ───────────────────────────────────────────────────────── */

    /** Runs on the dispatcher thread, in submission order. After close() it runs inline. */
    private void dispatch(Runnable r) {
        try {
            events.execute(r);
        } catch (RejectedExecutionException e) {
            safely(r::run);
        }
    }

    private void offerProgress(ProgressInfo p) {
        /* put() == null: nothing is scheduled for this download yet, so schedule
         * one delivery. Otherwise the scheduled delivery will pick up the newer
         * value this put() just stored. */
        if (latestProgress.put(p.id(), p) == null) {
            long id = p.id();
            dispatch(() -> {
                ProgressInfo newest = latestProgress.remove(id);
                if (newest != null) notifyProgress(newest);
            });
        }
    }

    private interface Call { void run() throws Exception; }

    private static void safely(Call c) {
        try {
            c.run();
        } catch (Throwable t) {
            log.error("fluxdm-ipc listener failed", t);
        }
    }

    private void notifyProgress(ProgressInfo p) { for (IPCEventListener l : listeners) safely(() -> l.onProgress(p)); }
    private void notifyComplete(CompleteInfo c) { for (IPCEventListener l : listeners) safely(() -> l.onComplete(c)); }
    private void notifyStatus(StatusInfo s)     { for (IPCEventListener l : listeners) safely(() -> l.onStatus(s)); }
    private void notifyError(long id, int code, String msg) {
        for (IPCEventListener l : listeners) safely(() -> l.onError(id, code, msg));
    }

    private static String cString(byte[] b, int offset, int end) {
        int len = 0;
        while (offset + len < end && b[offset + len] != 0) len++;
        return new String(b, offset, len, StandardCharsets.UTF_8);
    }

    /* ── Parsed payloads — layouts match ipc/messages.h exactly. Ids are
     *    unsigned 32-bit on the wire and are exposed as long. ──────────────── */

    /** Outcome of one command. */
    public record AckResult(boolean ok, int errorCode, String errorMessage, List<StatusInfo> statuses) {
        public static AckResult success()                        { return new AckResult(true, 0, null, List.of()); }
        public static AckResult success(List<StatusInfo> rows)   { return new AckResult(true, 0, null, rows); }
        public static AckResult failure(int code, String msg)    { return new AckResult(false, code, msg, List.of()); }

        /** The core does not know that download id. */
        public boolean isNotFound()       { return !ok && errorCode == ERR_NOT_FOUND; }
        /** The core knows the download but it is in the wrong state for the command. */
        public boolean isBadState()       { return !ok && errorCode == ERR_INTERNAL; }
        /** There is no working connection to the core (never reached it, or lost it). */
        public boolean coreUnreachable()  { return !ok && (errorCode == ERR_NOT_CONNECTED || errorCode == ERR_CONNECTION_LOST); }
        /** The core did not answer in time. */
        public boolean noReply()          { return !ok && errorCode == ERR_NO_REPLY; }
        /** The failure originated on this side of the socket. */
        public boolean clientSide()       { return !ok && errorCode < 0; }
    }

    /** flux_rsp_progress_t (32 bytes). */
    public record ProgressInfo(long id, long totalBytes, long downloadedBytes,
                               long speedBps, long etaSeconds, int activeSegments, int status) {
        static final int SIZE = 32;

        public boolean etaKnown() { return etaSeconds != ETA_UNKNOWN; }

        static ProgressInfo parse(byte[] p) {
            if (p.length < SIZE) return null;
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            long id         = Integer.toUnsignedLong(b.getInt());
            long total      = b.getLong();
            long downloaded = b.getLong();
            long speed      = Integer.toUnsignedLong(b.getInt());
            long eta        = Integer.toUnsignedLong(b.getInt());
            int  active     = b.get() & 0xFF;
            int  status     = b.get() & 0xFF;
            return new ProgressInfo(id, total, downloaded, speed, eta, active, status);
        }
    }

    /** flux_rsp_complete_t (52 bytes). */
    public record CompleteInfo(long id, long totalBytes, long elapsedMs,
                               int checksumOk, byte[] checksum) {
        static final int SIZE = 52;

        static CompleteInfo parse(byte[] p) {
            if (p.length < SIZE) return null;
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            long id      = Integer.toUnsignedLong(b.getInt());
            long total   = b.getLong();
            long elapsed = Integer.toUnsignedLong(b.getInt());
            int  ok      = b.get() & 0xFF;
            byte[] cksum = new byte[32];
            b.get(cksum);
            return new CompleteInfo(id, total, elapsed, ok, cksum);
        }
    }

    /** flux_rsp_status_t (32 bytes). */
    public record StatusInfo(long id, int status, int activeSegments, int totalSegments,
                             long totalBytes, long downloadedBytes,
                             long speedBps, long etaSeconds) {
        static final int SIZE = 32;

        public boolean etaKnown() { return etaSeconds != ETA_UNKNOWN; }

        static StatusInfo parse(byte[] p) {
            if (p.length < SIZE) return null;
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            long id         = Integer.toUnsignedLong(b.getInt());
            int  status     = b.get() & 0xFF;
            int  active     = b.get() & 0xFF;
            int  totalSegs  = b.get() & 0xFF;
            b.get();                                   /* _pad */
            long total      = b.getLong();
            long downloaded = b.getLong();
            long speed      = Integer.toUnsignedLong(b.getInt());
            long eta        = Integer.toUnsignedLong(b.getInt());
            return new StatusInfo(id, status, active, totalSegs, total, downloaded, speed, eta);
        }
    }
}
