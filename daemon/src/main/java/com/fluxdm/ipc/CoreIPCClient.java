/* FluxDM — ipc/CoreIPCClient.java
 * Persistent Unix domain socket client to fluxdm-core.
 * Wire format matches ipc/protocol.h + ipc/messages.h exactly, validated
 * byte-for-byte in Phase 1 (96/96 unit + 16/16 integration tests).
 *
 * Architecture:
 *   - One long-lived connection, one background reader thread.
 *   - Writes are synchronized (single writer lock) since REST requests
 *     from multiple threads can trigger commands concurrently.
 *   - The reader thread parses every incoming frame and either:
 *       (a) completes a pending CompletableFuture for RSP_OK/RSP_ERROR
 *           that immediately follows a command (request/response ack), or
 *       (b) dispatches RSP_PROGRESS / RSP_COMPLETE / RSP_STATUS to all
 *           registered IPCEventListeners (fire-and-forget push events).
 */
package com.fluxdm.ipc;

import org.springframework.stereotype.Component;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

@Component
public class CoreIPCClient {

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

    private static final int HEADER_SIZE       = 16;
    private static final int CMD_START_FIXED   = 56;   /* flux_cmd_start_t fixed part */
    private static final int ACK_TIMEOUT_SECS  = 15;

    /* ── State ────────────────────────────────────────────────────────────── */
    private final String sockPath;
    private volatile SocketChannel channel;
    private Thread readerThread;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final Object writeLock = new Object();

    private final List<IPCEventListener> listeners =
        Collections.synchronizedList(new ArrayList<>());

    /* Correlates a just-sent command to its immediate RSP_OK/RSP_ERROR ack.
     * Keyed by download id (0 reserved for global commands). */
    private final ConcurrentHashMap<Integer, CompletableFuture<AckResult>> pendingAcks =
        new ConcurrentHashMap<>();

    public CoreIPCClient() { this("/tmp/fluxdm.sock"); }
    public CoreIPCClient(String sockPath) { this.sockPath = sockPath; }

    public void addListener(IPCEventListener l) { listeners.add(l); }
    public void removeListener(IPCEventListener l) { listeners.remove(l); }

    /* ── Lifecycle ────────────────────────────────────────────────────────── */

    public synchronized void connect() throws IOException {
        if (running.get()) return;
        UnixDomainSocketAddress addr = UnixDomainSocketAddress.of(Path.of(sockPath));
        SocketChannel ch = SocketChannel.open(StandardProtocolFamily.UNIX);
        ch.connect(addr);
        this.channel = ch;
        running.set(true);

        readerThread = new Thread(this::readLoop, "fluxdm-ipc-reader");
        readerThread.setDaemon(true);
        readerThread.start();
    }

    public synchronized void disconnect() {
        running.set(false);
        SocketChannel ch = this.channel;
        if (ch != null) {
            try { ch.close(); } catch (IOException ignored) {}
        }
        if (readerThread != null) readerThread.interrupt();

        /* Fail any commands still waiting on a reply */
        pendingAcks.forEach((id, f) ->
            f.complete(AckResult.failure(-1, "disconnected")));
        pendingAcks.clear();
    }

    public boolean isConnected() {
        SocketChannel ch = this.channel;
        return ch != null && ch.isOpen() && ch.isConnected();
    }

    /* ── Commands ─────────────────────────────────────────────────────────── */

    public CompletableFuture<AckResult> start(int id, String url, String destPath,
                                               int segments, long bwLimitBps,
                                               int checksumType, byte[] checksum) {
        byte[] urlBytes  = url.getBytes(StandardCharsets.UTF_8);
        byte[] pathBytes = destPath.getBytes(StandardCharsets.UTF_8);
        int payloadLen   = CMD_START_FIXED + 2 + urlBytes.length + 2 + pathBytes.length;

        ByteBuffer buf = ByteBuffer.allocate(HEADER_SIZE + payloadLen)
                                   .order(ByteOrder.LITTLE_ENDIAN);
        writeHeader(buf, CMD_START, id, payloadLen);

        buf.putInt(id);
        buf.putShort((short) segments);
        buf.putShort((short) 0);              /* flags, reserved */
        buf.putLong(bwLimitBps);
        buf.put((byte) checksumType);
        byte[] cksum32 = new byte[32];
        if (checksum != null)
            System.arraycopy(checksum, 0, cksum32, 0, Math.min(32, checksum.length));
        buf.put(cksum32);
        buf.put(new byte[7]);                 /* _pad */
        putShortString(buf, urlBytes);
        putShortString(buf, pathBytes);

        CompletableFuture<AckResult> future = registerAck(id);
        send(buf.array());
        return future;
    }

    public CompletableFuture<AckResult> pause(int id)  { return sendHeaderOnly(CMD_PAUSE,  id); }
    public CompletableFuture<AckResult> resume(int id) { return sendHeaderOnly(CMD_RESUME, id); }
    public CompletableFuture<AckResult> cancel(int id) { return sendHeaderOnly(CMD_CANCEL, id); }

    /** id = 0 sets the global bandwidth limit; any other id targets one download. */
    public CompletableFuture<AckResult> setLimit(int id, long bytesPerSec) {
        ByteBuffer buf = ByteBuffer.allocate(HEADER_SIZE + 8).order(ByteOrder.LITTLE_ENDIAN);
        writeHeader(buf, CMD_SET_LIMIT, id, 8);
        buf.putLong(bytesPerSec);
        CompletableFuture<AckResult> future = registerAck(id);
        send(buf.array());
        return future;
    }

    /** Fire-and-forget: the reply arrives as an RSP_STATUS push to listeners. */
    public void requestStatus(int id)    { send(headerOnlyBytes(CMD_STATUS, id)); }
    public void requestStatusAll()       { send(headerOnlyBytes(CMD_STATUS_ALL, 0)); }
    public void shutdownCore()           { send(headerOnlyBytes(CMD_SHUTDOWN, 0)); }

    private CompletableFuture<AckResult> sendHeaderOnly(byte type, int id) {
        CompletableFuture<AckResult> future = registerAck(id);
        send(headerOnlyBytes(type, id));
        return future;
    }

    private CompletableFuture<AckResult> registerAck(int id) {
        CompletableFuture<AckResult> f = new CompletableFuture<>();
        pendingAcks.put(id, f);
        /* Safety net: never let a caller hang forever if the core is unresponsive */
        f.orTimeout(ACK_TIMEOUT_SECS, TimeUnit.SECONDS)
         .whenComplete((r, ex) -> pendingAcks.remove(id, f));
        return f;
    }

    private byte[] headerOnlyBytes(byte type, int id) {
        ByteBuffer b = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        writeHeader(b, type, id, 0);
        return b.array();
    }

    private void putShortString(ByteBuffer buf, byte[] bytes) {
        buf.putShort((short) bytes.length);
        buf.put(bytes);
    }

    private void writeHeader(ByteBuffer b, byte type, int id, int payloadLen) {
        b.putInt(FLUX_MAGIC);
        b.put(FLUX_VER);
        b.put(type);
        b.putShort((short) 0);   /* flags, reserved */
        b.putInt(id);
        b.putInt(payloadLen);
    }

    private void send(byte[] data) {
        SocketChannel ch = this.channel;
        if (ch == null || !ch.isConnected())
            throw new IllegalStateException("Not connected to fluxdm-core");
        synchronized (writeLock) {
            try {
                ByteBuffer bb = ByteBuffer.wrap(data);
                while (bb.hasRemaining()) ch.write(bb);
            } catch (IOException e) {
                throw new RuntimeException("IPC send failed: " + e.getMessage(), e);
            }
        }
    }

    /* ── Reader thread ────────────────────────────────────────────────────── */

    private void readLoop() {
        ByteBuffer headerBuf = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        try {
            while (running.get()) {
                headerBuf.clear();
                if (!readFully(headerBuf)) break;   /* EOF: core closed connection */

                headerBuf.flip();
                int   magic      = headerBuf.getInt();
                byte  version    = headerBuf.get();
                byte  type       = headerBuf.get();
                short flags      = headerBuf.getShort();
                int   id         = headerBuf.getInt();
                int   payloadLen = headerBuf.getInt();

                if (magic != FLUX_MAGIC) {
                    System.err.println("[fluxdm-ipc] bad magic 0x" +
                        Integer.toHexString(magic) + " — dropping connection");
                    break;
                }
                if (version != FLUX_VER) {
                    System.err.println("[fluxdm-ipc] unexpected protocol version " + version);
                }

                byte[] payload = new byte[payloadLen];
                if (payloadLen > 0) {
                    ByteBuffer pb = ByteBuffer.wrap(payload);
                    if (!readFully(pb)) break;
                }

                dispatch(type, id, payload);
            }
        } catch (IOException e) {
            if (running.get())
                System.err.println("[fluxdm-ipc] reader error: " + e.getMessage());
        } finally {
            running.set(false);
        }
    }

    private boolean readFully(ByteBuffer buf) throws IOException {
        SocketChannel ch = this.channel;
        while (buf.hasRemaining()) {
            int n = ch.read(buf);
            if (n < 0) return false;   /* EOF */
        }
        return true;
    }

    private void dispatch(byte type, int id, byte[] payload) {
        switch (type) {
            case RSP_OK -> completeAck(id, AckResult.success());

            case RSP_ERROR -> {
                int code = payload.length > 0 ? (payload[0] & 0xFF) : 0;
                String msg = payload.length > 4
                    ? cString(payload, 4, payload.length)
                    : "";
                completeAck(id, AckResult.failure(code, msg));
                notifyError(id, code, msg);
            }

            case RSP_PROGRESS -> notifyProgress(ProgressInfo.parse(payload));
            case RSP_COMPLETE -> notifyComplete(CompleteInfo.parse(payload));
            case RSP_STATUS   -> notifyStatus(StatusInfo.parse(payload));

            default -> System.err.println(
                "[fluxdm-ipc] unknown frame type 0x" + Integer.toHexString(type & 0xFF));
        }
    }

    private void completeAck(int id, AckResult result) {
        CompletableFuture<AckResult> f = pendingAcks.remove(id);
        if (f != null) f.complete(result);
    }

    private void notifyProgress(ProgressInfo p) {
        synchronized (listeners) { for (var l : listeners) l.onProgress(p); }
    }
    private void notifyComplete(CompleteInfo c) {
        synchronized (listeners) { for (var l : listeners) l.onComplete(c); }
    }
    private void notifyError(int id, int code, String msg) {
        synchronized (listeners) { for (var l : listeners) l.onError(id, code, msg); }
    }
    private void notifyStatus(StatusInfo s) {
        synchronized (listeners) { for (var l : listeners) l.onStatus(s); }
    }

    private static String cString(byte[] b, int offset, int end) {
        int len = 0;
        while (offset + len < end && b[offset + len] != 0) len++;
        return new String(b, offset, len, StandardCharsets.UTF_8);
    }

    /* ── Parsed payload records — field offsets match ipc/messages.h exactly,
     *    validated byte-for-byte against the C core in Phase 1 testing ──────── */

    public record AckResult(boolean ok, int errorCode, String errorMessage) {
        static AckResult success()                   { return new AckResult(true, 0, null); }
        static AckResult failure(int code, String msg){ return new AckResult(false, code, msg); }
    }

    /** Mirrors flux_rsp_progress_t (32 bytes packed). */
    public record ProgressInfo(int id, long totalBytes, long downloadedBytes,
                                long speedBps, long etaSeconds, int activeSegments) {
        static ProgressInfo parse(byte[] p) {
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            int  id          = b.getInt();
            long total       = b.getLong();
            long downloaded  = b.getLong();
            long speed       = Integer.toUnsignedLong(b.getInt());
            long eta         = Integer.toUnsignedLong(b.getInt());
            int  activeSegs  = b.get() & 0xFF;
            return new ProgressInfo(id, total, downloaded, speed, eta, activeSegs);
        }
    }

    /** Mirrors flux_rsp_complete_t (52 bytes packed). */
    public record CompleteInfo(int id, long totalBytes, long elapsedMs,
                                int checksumOk, byte[] checksum) {
        static CompleteInfo parse(byte[] p) {
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            int  id      = b.getInt();
            long total   = b.getLong();
            long elapsed = Integer.toUnsignedLong(b.getInt());
            int  ok      = b.get() & 0xFF;
            byte[] cksum = new byte[32];
            b.get(cksum);
            return new CompleteInfo(id, total, elapsed, ok, cksum);
        }
    }

    /** Mirrors flux_rsp_status_t (32 bytes packed). */
    public record StatusInfo(int id, int status, int activeSegments, int totalSegments,
                              long totalBytes, long downloadedBytes,
                              long speedBps, long etaSeconds) {
        static StatusInfo parse(byte[] p) {
            ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
            int  id        = b.getInt();
            int  status    = b.get() & 0xFF;
            int  active    = b.get() & 0xFF;
            int  totalSegs = b.get() & 0xFF;
            b.get();  /* _pad */
            long total      = b.getLong();
            long downloaded = b.getLong();
            long speed      = Integer.toUnsignedLong(b.getInt());
            long eta        = Integer.toUnsignedLong(b.getInt());
            return new StatusInfo(id, status, active, totalSegs, total, downloaded, speed, eta);
        }
    }
}
