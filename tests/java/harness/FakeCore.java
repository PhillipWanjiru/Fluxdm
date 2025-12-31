/* FluxDM — tests/java/harness/FakeCore.java
 * A scriptable stand-in for fluxdm-core that speaks the real wire protocol
 * (ipc/protocol.h + ipc/messages.h) and follows the real core's command
 * semantics (core/src/ipc/socket_server.c, core/src/downloader.c):
 *
 *   START    OK, or ERROR for id 0 / a download already running / a destination in use;
 *            a START for the id of a finished, failed or paused download replaces it
 *   PAUSE    OK if running or already paused; NOT_FOUND; otherwise BAD_STATE (INTERNAL)
 *   RESUME   OK if running; relaunches paused/failed; NOT_FOUND; BAD_STATE otherwise
 *   CANCEL   always OK (idempotent), forgets the download
 *   SET_LIMIT  OK; NOT_FOUND for an unknown id other than 0
 *   STATUS   one status frame or NOT_FOUND; STATUS_ALL = rows, then OK(0)
 *   SHUTDOWN OK(0), then closes
 *
 * Replies carry flags 0 and are written in the order the commands were read;
 * events (progress, complete, async error) carry FLUX_FLAG_EVENT.
 * The test drives the "download" side by hand: emitProgress/emitComplete/emitError.
 */
package harness;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;

public final class FakeCore implements AutoCloseable {

    public static final int MAGIC = 0x464C5558;

    public static final int START = 0x01, PAUSE = 0x02, RESUME = 0x03, CANCEL = 0x04,
                            SET_LIMIT = 0x05, STATUS = 0x06, STATUS_ALL = 0x07, SHUTDOWN = 0xFF;
    public static final int RSP_OK = 0x10, RSP_ERROR = 0x11, RSP_PROGRESS = 0x12,
                            RSP_COMPLETE = 0x13, RSP_STATUS = 0x14;

    public static final int IDLE = 0, CONNECTING = 1, DOWNLOADING = 2, PAUSED = 3, COMPLETE = 4, ERROR = 5;

    public static final int E_NOT_FOUND = 6, E_INTERNAL = 99;

    /** A command as the core received it. */
    public record Cmd(int type, long id, byte[] payload) {}

    /** The core's record of one download. */
    public static final class Dl {
        public final long id;
        public volatile int state = DOWNLOADING;
        public volatile String url = "", path = "";
        public volatile long total, done, limit;
        public volatile int segments, cksumType;
        public byte[] cksum = new byte[32];

        public Dl(long id) { this.id = id; }
        boolean alive() { return state == DOWNLOADING || state == CONNECTING || state == IDLE; }
    }

    private final Path sockPath;
    private volatile ServerSocketChannel server;
    private volatile SocketChannel client;
    private volatile boolean closed;
    private volatile boolean muted;
    private volatile boolean swallowCancel;
    private volatile long replyDelayMs;
    private volatile long globalLimit;
    private int acceptCount;                              /* guarded by lifecycle */
    private final Object lifecycle = new Object();
    private final List<SocketChannel> accepted = new ArrayList<>();   /* guarded by lifecycle */

    /** Called on the serving thread right after a command was answered. */
    public volatile java.util.function.Consumer<Cmd> afterReply;

    public final List<Cmd> commands = new CopyOnWriteArrayList<>();
    private final Map<Long, Dl> registry = new ConcurrentHashMap<>();
    private final Object writeLock = new Object();

    public FakeCore(Path sockPath) throws IOException {
        this.sockPath = sockPath;
        listen();
    }

    /* ── Lifecycle ───────────────────────────────────────────────────────── */

    private void listen() throws IOException {
        synchronized (lifecycle) {
            Files.deleteIfExists(sockPath);
            ServerSocketChannel s = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
            s.bind(UnixDomainSocketAddress.of(sockPath));
            server = s;
            Thread t = new Thread(() -> acceptLoop(s), "fakecore-accept");
            t.setDaemon(true);
            t.start();
        }
    }

    private void acceptLoop(ServerSocketChannel s) {
        while (!closed && s.isOpen()) {
            SocketChannel c;
            try {
                c = s.accept();
            } catch (IOException e) {
                return;
            }
            synchronized (lifecycle) {
                if (server != s) {                       /* killed while this connection was being accepted */
                    quietly(c);
                    return;
                }
                accepted.add(c);
                acceptCount++;
                SocketChannel old = client;
                client = c;
                if (old != null) quietly(old);
            }
            Thread t = new Thread(() -> serve(c), "fakecore-serve");
            t.setDaemon(true);
            t.start();
        }
    }

    private static void quietly(java.nio.channels.Channel c) {
        try { if (c != null) c.close(); } catch (IOException ignored) { /* already closed */ }
    }

    public int accepts() { synchronized (lifecycle) { return acceptCount; } }

    /** The core dies: every connection and the socket gone, every download and the global limit forgotten. */
    public void kill() {
        synchronized (lifecycle) {
            ServerSocketChannel s = server;
            server = null;
            client = null;
            for (SocketChannel c : accepted) quietly(c);
            accepted.clear();
            quietly(s);
            try { Files.deleteIfExists(sockPath); } catch (IOException ignored) { /* best effort */ }
            registry.clear();
            globalLimit = 0;
        }
    }

    /** A fresh core comes up on the same path, knowing nothing. */
    public void restart() throws IOException {
        kill();
        listen();
    }

    /** The connection drops but the core (and its downloads) live on. */
    public void dropClient() {
        synchronized (lifecycle) {
            SocketChannel c = client;
            client = null;
            if (c != null) accepted.remove(c);
            quietly(c);
        }
    }

    @Override public void close() {
        closed = true;
        kill();
    }

    public boolean hasClient() { SocketChannel c = client; return c != null && c.isOpen(); }

    /* ── Test controls ───────────────────────────────────────────────────── */

    /** Read commands but send no replies at all. */
    public void mute(boolean on) { muted = on; }

    /** CANCEL commands are read and recorded but neither applied nor answered (a command that never got through). */
    public void swallowCancel(boolean on) { swallowCancel = on; }

    /** Sleep this long before each reply (the reader thread, so later commands wait too). */
    public void replyDelay(long ms) { replyDelayMs = ms; }

    public Dl dl(long id) { return registry.get(id); }
    /** The core silently loses a download (restart without losing the connection, retention expiry). */
    public void forget(long id) { registry.remove(id); }
    public Dl put(long id, int state, long total, long done) {
        Dl d = new Dl(id);
        d.state = state; d.total = total; d.done = done;
        registry.put(id, d);
        return d;
    }
    public long globalLimit() { return globalLimit; }

    public List<Cmd> commands(int type) {
        List<Cmd> out = new ArrayList<>();
        for (Cmd c : commands) if (c.type == type) out.add(c);
        return out;
    }
    public List<Cmd> commands(int type, long id) {
        List<Cmd> out = new ArrayList<>();
        for (Cmd c : commands) if (c.type == type && c.id == id) out.add(c);
        return out;
    }
    public int count(int type, long id) { return commands(type, id).size(); }

    public void emitProgress(long id, long done, long total, long speed) {
        Dl d = registry.get(id);
        if (d != null) { d.done = done; d.total = total; }
        int st = d != null ? d.state : DOWNLOADING;
        ByteBuffer b = le(32);
        b.putInt((int) id).putLong(total).putLong(done).putInt((int) speed).putInt(1)
         .put((byte) 4).put((byte) st).put(new byte[2]);
        send(RSP_PROGRESS, 1, id, b.array());
    }

    public void emitComplete(long id, long total) {
        Dl d = registry.get(id);
        if (d != null) { d.state = COMPLETE; d.done = total; d.total = total; }
        ByteBuffer b = le(52);
        b.putInt((int) id).putLong(total).putInt(1234).put((byte) 1).put(new byte[32]).put(new byte[3]);
        send(RSP_COMPLETE, 1, id, b.array());
    }

    public void emitError(long id, int code, String msg) {
        Dl d = registry.get(id);
        if (d != null) d.state = ERROR;
        send(RSP_ERROR, 1, id, errorPayload(code, msg));
    }

    /** Raw bytes straight onto the connection (protocol-violation tests). */
    public void injectRaw(byte[] bytes) {
        SocketChannel c = awaitClient(2000);
        if (c == null) return;
        synchronized (writeLock) {
            try {
                ByteBuffer bb = ByteBuffer.wrap(bytes);
                while (bb.hasRemaining()) c.write(bb);
            } catch (IOException ignored) { /* client gone */ }
        }
    }

    public void injectFrame(int type, int flags, long id, byte[] payload) { send(type, flags, id, payload); }

    /* ── Serving ─────────────────────────────────────────────────────────── */

    private void serve(SocketChannel c) {
        try {
            serveLoop(c);
        } finally {
            synchronized (lifecycle) {
                accepted.remove(c);
                if (client == c) client = null;
            }
            quietly(c);
        }
    }

    private void serveLoop(SocketChannel c) {
        ByteBuffer hdr = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        try {
            while (true) {
                hdr.clear();
                if (!readFully(c, hdr)) return;
                hdr.flip();
                int  magic = hdr.getInt();
                hdr.get();                                   /* version */
                int  type  = hdr.get() & 0xFF;
                hdr.getShort();                              /* flags */
                long id    = hdr.getInt() & 0xFFFFFFFFL;
                int  plen  = hdr.getInt();
                if (magic != MAGIC || plen < 0 || plen > (1 << 20)) return;
                byte[] payload = new byte[plen];
                if (plen > 0 && !readFully(c, ByteBuffer.wrap(payload))) return;

                Cmd cmd = new Cmd(type, id, payload);
                commands.add(cmd);
                if (muted) continue;
                if (replyDelayMs > 0) T.sleep(replyDelayMs);
                handle(cmd, c);
                java.util.function.Consumer<Cmd> hook = afterReply;
                if (hook != null) hook.accept(cmd);
                if (type == SHUTDOWN) { try { c.close(); } catch (IOException ignored) { /* done */ } return; }
            }
        } catch (IOException e) {
            /* client went away */
        }
    }

    private static boolean readFully(SocketChannel c, ByteBuffer b) throws IOException {
        while (b.hasRemaining()) if (c.read(b) < 0) return false;
        return true;
    }

    private synchronized void handle(Cmd cmd, SocketChannel c) {
        long id = cmd.id;
        Dl d = registry.get(id);
        switch (cmd.type) {
            case START -> {
                ByteBuffer b = ByteBuffer.wrap(cmd.payload).order(ByteOrder.LITTLE_ENDIAN);
                if (cmd.payload.length < 58) { err(c, id, E_INTERNAL, "CMD_START payload too short"); return; }
                b.getInt();
                int segs = b.getShort() & 0xFFFF;
                b.getShort();
                long limit = b.getLong();
                int ctype = b.get() & 0xFF;
                byte[] ck = new byte[32]; b.get(ck); b.get(new byte[7]);
                int ul = b.getShort() & 0xFFFF; byte[] ub = new byte[ul]; b.get(ub);
                int pl = b.getShort() & 0xFFFF; byte[] pb = new byte[pl]; b.get(pb);
                String url = new String(ub, StandardCharsets.UTF_8), path = new String(pb, StandardCharsets.UTF_8);

                if (id == 0)                  { err(c, id, E_INTERNAL, "download id 0 is reserved for daemon-wide commands"); return; }
                if (d != null && d.alive())   { err(c, id, E_INTERNAL, "download " + id + " is already running"); return; }
                for (Dl o : registry.values())
                    if (o.id != id && o.path.equals(path) && (o.alive() || o.state == PAUSED)) {
                        err(c, id, E_INTERNAL, "the destination file is already used by download " + o.id);
                        return;
                    }
                Dl nd = new Dl(id);
                nd.url = url; nd.path = path; nd.limit = limit; nd.segments = segs; nd.cksumType = ctype; nd.cksum = ck;
                nd.total = 1_000_000;
                registry.put(id, nd);
                reply(c, RSP_OK, id, null);
            }
            case PAUSE -> {
                if (d == null)                 { err(c, id, E_NOT_FOUND, "no such download"); return; }
                if (d.state == PAUSED)         { reply(c, RSP_OK, id, null); return; }
                if (!d.alive())                { err(c, id, E_INTERNAL, "download " + id + " is " + stateName(d.state) + " and cannot be paused"); return; }
                d.state = PAUSED;
                reply(c, RSP_OK, id, null);
            }
            case RESUME -> {
                if (d == null)                 { err(c, id, E_NOT_FOUND, "no such download"); return; }
                if (d.alive())                 { reply(c, RSP_OK, id, null); return; }
                if (d.state != PAUSED && d.state != ERROR) { err(c, id, E_INTERNAL, "download " + id + " is " + stateName(d.state) + " and cannot be resumed"); return; }
                d.state = DOWNLOADING;
                reply(c, RSP_OK, id, null);
            }
            case CANCEL -> {
                if (swallowCancel) return;
                registry.remove(id);
                reply(c, RSP_OK, id, null);
            }
            case SET_LIMIT -> {
                long bps = ByteBuffer.wrap(cmd.payload).order(ByteOrder.LITTLE_ENDIAN).getLong();
                if (id == 0) globalLimit = bps;
                else if (d != null) d.limit = bps;
                else { err(c, id, E_NOT_FOUND, "no such download"); return; }
                reply(c, RSP_OK, id, null);
            }
            case STATUS -> {
                if (d == null) { err(c, id, E_NOT_FOUND, "no such download"); return; }
                reply(c, RSP_STATUS, id, statusPayload(d));
            }
            case STATUS_ALL -> {
                for (Dl x : new ArrayList<>(registry.values())) reply(c, RSP_STATUS, x.id, statusPayload(x));
                reply(c, RSP_OK, 0, null);
            }
            case SHUTDOWN -> reply(c, RSP_OK, 0, null);
            default -> err(c, id, E_INTERNAL, "unknown command 0x" + Integer.toHexString(cmd.type));
        }
    }

    private static String stateName(int s) {
        return switch (s) { case COMPLETE -> "complete"; case ERROR -> "error"; case PAUSED -> "paused"; default -> "running"; };
    }

    /* ── Frames ──────────────────────────────────────────────────────────── */

    /** A reply goes back on the connection the command arrived on — never to whichever client is current. */
    private void reply(SocketChannel to, int type, long id, byte[] payload) { write(to, type, 0, id, payload); }
    private void err(SocketChannel to, long id, int code, String msg)       { write(to, RSP_ERROR, 0, id, errorPayload(code, msg)); }

    private static ByteBuffer le(int n) { return ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN); }

    private static byte[] errorPayload(int code, String msg) {
        ByteBuffer b = le(128);
        b.put((byte) code).put(new byte[3]);
        byte[] m = msg.getBytes(StandardCharsets.UTF_8);
        b.put(m, 0, Math.min(m.length, 123));
        return b.array();
    }

    private static byte[] statusPayload(Dl d) {
        ByteBuffer b = le(32);
        b.putInt((int) d.id).put((byte) d.state).put((byte) (d.alive() ? 4 : 0)).put((byte) 4).put((byte) 0)
         .putLong(d.total).putLong(d.done).putInt(d.alive() ? 1000 : 0).putInt(5);
        return b.array();
    }

    /** An event (or injected frame) for whoever is connected right now. Waits briefly for a client to
     *  show up so a test that has just called connect() does not race the accept. */
    private void send(int type, int flags, long id, byte[] payload) {
        SocketChannel c = awaitClient(2000);
        if (c != null) write(c, type, flags, id, payload);
    }

    /** The current client, waiting up to timeoutMs for one to be accepted. null if none arrived. */
    private SocketChannel awaitClient(long timeoutMs) {
        long end = System.nanoTime() + timeoutMs * 1_000_000L;
        while (true) {
            SocketChannel c = client;
            if (c != null) return c;
            if (closed || server == null || System.nanoTime() >= end) return null;
            T.sleep(2);
        }
    }

    /** Blocks until the n-th connection since this core was created has been accepted. */
    public boolean awaitAccepts(int n, long timeoutMs) {
        return T.waitFor(() -> accepts() >= n, timeoutMs);
    }

    private void write(SocketChannel c, int type, int flags, long id, byte[] payload) {
        byte[] body = payload == null ? new byte[0] : payload;
        ByteBuffer f = le(16 + body.length);
        f.putInt(MAGIC).put((byte) 1).put((byte) type).putShort((short) flags).putInt((int) id).putInt(body.length).put(body);
        synchronized (writeLock) {
            try {
                ByteBuffer bb = ByteBuffer.wrap(f.array());
                while (bb.hasRemaining()) c.write(bb);
            } catch (IOException e) {
                /* the peer is gone (dropped on purpose, or a crash): nothing to deliver to */
            }
        }
    }
}
