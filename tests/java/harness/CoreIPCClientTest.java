/* FluxDM — tests/java/harness/CoreIPCClientTest.java
 * The real CoreIPCClient against FakeCore: reply matching, replies vs events,
 * ordering, coalescing, connection loss, timeouts and protocol violations. */
package harness;

import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.CoreIPCClient.AckResult;
import com.fluxdm.ipc.CoreIPCClient.CompleteInfo;
import com.fluxdm.ipc.CoreIPCClient.ProgressInfo;
import com.fluxdm.ipc.CoreIPCClient.StatusInfo;
import com.fluxdm.ipc.IPCEventListener;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public class CoreIPCClientTest {

    static final class Rec implements IPCEventListener {
        final List<ProgressInfo> progress = new CopyOnWriteArrayList<>();
        final List<CompleteInfo> complete = new CopyOnWriteArrayList<>();
        final List<StatusInfo>   status   = new CopyOnWriteArrayList<>();
        final List<String>       errors   = new CopyOnWriteArrayList<>();   /* "id:code:msg" */
        final List<String>       order    = new CopyOnWriteArrayList<>();
        final AtomicInteger connected = new AtomicInteger(), lost = new AtomicInteger();
        volatile String lostReason;
        volatile CountDownLatch gate;

        @Override public void onProgress(ProgressInfo i) { progress.add(i); order.add("progress:" + i.id() + ":" + i.downloadedBytes()); }
        @Override public void onComplete(CompleteInfo i) { complete.add(i); order.add("complete:" + i.id()); }
        @Override public void onError(long id, int code, String msg) { errors.add(id + ":" + code + ":" + msg); order.add("error:" + id); }
        @Override public void onStatus(StatusInfo i) {
            status.add(i);
            CountDownLatch g = gate;
            if (g != null) try { g.await(10, TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        @Override public void onConnected() { connected.incrementAndGet(); order.add("connected"); }
        @Override public void onConnectionLost(String reason) { lost.incrementAndGet(); lostReason = reason; order.add("lost"); }
    }

    static int sockSeq;
    static Path sock() { return Path.of("/tmp/fxj" + ProcessHandle.current().pid() + "_" + (sockSeq++) + ".sock"); }

    static AckResult get(CompletableFuture<AckResult> f) throws Exception { return f.get(5, TimeUnit.SECONDS); }

    static byte[] eventStatusFrameFor(long id) {
        ByteBuffer b = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt((int) id).put((byte) 2).put((byte) 1).put((byte) 1).put((byte) 0).putLong(100).putLong(50).putInt(10).putInt(5);
        return b.array();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM — CoreIPCClient protocol tests (against a scripted fake core)");

        basics();
        repliesVersusEvents();
        orderingAndCoalescing();
        concurrentCommands();
        statusCommands();
        connectionLossAndReconnect();
        timeouts();
        protocolViolations();
        argumentChecks();
        listenerIsolationAndClose();

        System.exit(T.finish("CoreIPCClient"));
    }

    /* ─────────────────────────────────────────────────────────────────── */

    static void basics() throws Exception {
        T.section("Connect, START on the wire, simple commands");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec early = new Rec();
            c.addListener(early);
            T.ok(!c.isConnected(), "not connected before connect()");
            AckResult nc = get(c.pause(1));
            T.ok(!nc.ok() && nc.coreUnreachable() && nc.errorCode() == CoreIPCClient.ERR_NOT_CONNECTED, "a command while disconnected fails at once with ERR_NOT_CONNECTED");

            c.connect();
            T.ok(c.isConnected(), "connected");
            T.within("listener registered before connect hears onConnected once", 2000, () -> early.connected.get() == 1);
            Rec late = new Rec();
            c.addListener(late);
            T.within("listener registered AFTER connect is told as well (replay)", 2000, () -> late.connected.get() == 1);
            T.eq(1, early.connected.get(), "...and the early one is not told twice");
            c.connect();
            T.eq(1, early.connected.get(), "connect() while connected is a no-op");

            byte[] cksum = new byte[32];
            for (int i = 0; i < 32; i++) cksum[i] = (byte) (0xA0 + i);
            AckResult r = get(c.start(4_000_000_000L, "https://example.com/a b.bin?x=1", "/tmp/out ü.bin", 6, 123456789012L, 2, cksum));
            T.ok(r.ok(), "START is acknowledged");
            FakeCore.Cmd cmd = core.commands(FakeCore.START).get(0);
            ByteBuffer b = ByteBuffer.wrap(cmd.payload()).order(ByteOrder.LITTLE_ENDIAN);
            T.eq(4_000_000_000L, cmd.id(), "header id is the unsigned 32-bit id (4e9 > 2^31)");
            T.eq(4_000_000_000L, b.getInt() & 0xFFFFFFFFL, "payload id matches");
            T.eq(6, b.getShort() & 0xFFFF, "segments");
            T.eq(0, b.getShort() & 0xFFFF, "flags are zero");
            T.eq(123456789012L, b.getLong(), "bandwidth limit (64-bit)");
            T.eq(2, b.get() & 0xFF, "checksum type");
            byte[] ck = new byte[32]; b.get(ck);
            T.ok(java.util.Arrays.equals(cksum, ck), "checksum bytes");
            b.get(new byte[7]);
            int ul = b.getShort() & 0xFFFF; byte[] ub = new byte[ul]; b.get(ub);
            int pl = b.getShort() & 0xFFFF; byte[] pb = new byte[pl]; b.get(pb);
            T.eq("https://example.com/a b.bin?x=1", new String(ub, StandardCharsets.UTF_8), "url (UTF-8, length-prefixed)");
            T.eq("/tmp/out ü.bin", new String(pb, StandardCharsets.UTF_8), "path (UTF-8, length-prefixed)");
            T.eq(b.remaining(), 0, "no stray bytes after the path");

            T.ok(get(c.pause(4_000_000_000L)).ok(), "PAUSE of a running download");
            T.ok(get(c.resume(4_000_000_000L)).ok(), "RESUME");
            T.ok(get(c.setLimit(0, 512_000)).ok() && core.globalLimit() == 512_000, "SET_LIMIT 0 sets the global limit");
            T.ok(get(c.setLimit(4_000_000_000L, 77)).ok() && core.dl(4_000_000_000L).limit == 77, "SET_LIMIT for one download");
            AckResult nf = get(c.pause(555));
            T.ok(nf.isNotFound() && !nf.clientSide(), "PAUSE of an unknown id: core error NOT_FOUND (code " + nf.errorCode() + ")");
            T.eq("no such download", nf.errorMessage(), "the core's message text arrives");
            T.ok(get(c.cancel(555)).ok(), "CANCEL is idempotent: OK even for an unknown id");
            T.ok(get(c.shutdownCore()).ok(), "SHUTDOWN is acknowledged");
            c.close();
        }
    }

    static void repliesVersusEvents() throws Exception {
        T.section("Replies and events are told apart by the flag, not by type or id");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            core.put(7, FakeCore.DOWNLOADING, 1000, 0);

            /* An async error EVENT for download 7 arrives while a PAUSE 7 is outstanding. Both are
             * RSP_ERROR frames for id 7; the core then answers the PAUSE with a REPLY error
             * ("cannot be paused": the download has failed meanwhile). */
            core.afterReply = null;
            core.replyDelay(300);                         /* hold the PAUSE reply back */
            CompletableFuture<AckResult> pause = c.pause(7);
            T.sleep(50);
            core.emitError(7, 2, "connection reset by peer");   /* event, flags=1, id 7, RSP_ERROR */
            T.within("the async error reaches the listener", 2000, () -> rec.errors.size() == 1);
            T.ok(!pause.isDone(), "...and does NOT complete the pending PAUSE");
            AckResult pr = get(pause);
            core.replyDelay(0);
            T.ok(!pr.ok() && pr.isBadState() && pr.errorMessage().contains("cannot be paused"),
                 "the PAUSE fails with ITS OWN reply, not the event's text (" + pr.errorMessage() + ")");
            T.eq(1, rec.errors.size(), "the reply error was not also reported as an async error");
            T.eq("7:2:connection reset by peer", rec.errors.get(0), "the event carries id, code and message");

            /* The mirror image: a reply ERROR (flags 0) goes to the future, not to listeners. */
            int before = rec.errors.size();
            AckResult nf = get(c.resume(808));
            T.ok(nf.isNotFound(), "a reply ERROR fails its command");
            T.eq(before, rec.errors.size(), "...and is NOT reported as an async error");

            /* Progress and completion are events. */
            core.emitProgress(7, 500, 1000, 4096);
            T.within("progress event", 2000, () -> rec.progress.size() == 1);
            ProgressInfo pi = rec.progress.get(0);
            T.ok(pi.id() == 7 && pi.downloadedBytes() == 500 && pi.totalBytes() == 1000 && pi.speedBps() == 4096
                 && pi.activeSegments() == 4 && pi.status() == CoreIPCClient.CORE_ERROR && pi.etaKnown() == true,
                 "progress fields incl. the status byte, which the old client dropped (" + pi + ")");
            core.emitComplete(7, 1000);
            T.within("complete event", 2000, () -> rec.complete.size() == 1);
            T.ok(rec.complete.get(0).id() == 7 && rec.complete.get(0).totalBytes() == 1000 && rec.complete.get(0).checksumOk() == 1
                 && rec.complete.get(0).elapsedMs() == 1234, "complete fields");
            c.close();
        }
    }

    static void orderingAndCoalescing() throws Exception {
        T.section("Ordering: a reply is seen before the events that followed it; progress is coalesced");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            T.within("connected", 2000, () -> rec.connected.get() == 1);

            /* Hold the dispatcher thread inside a listener so everything queues up behind it. */
            rec.gate = new CountDownLatch(1);
            core.injectFrame(FakeCore.RSP_STATUS, 1, 99, eventStatusFrameFor(99));      /* event: onStatus blocks */
            T.within("dispatcher is blocked in the listener", 2000, () -> rec.status.size() == 1);

            core.afterReply = cmd -> { if (cmd.type() == FakeCore.START) core.emitProgress(cmd.id(), 10, 100, 1); };
            List<String> seen = new CopyOnWriteArrayList<>();
            CompletableFuture<AckResult> f = c.start(5, "http://h/x", "/tmp/x", 4, 0, 0, null);
            f.thenAccept(r -> seen.add("ack"));
            rec.progress.clear();
            T.within("START reached the core and was answered", 2000, () -> core.count(FakeCore.START, 5) == 1);
            T.sleep(100);                                             /* the reply + progress are now queued behind the blocked callback */
            rec.gate.countDown();
            T.within("ack completed", 2000, () -> seen.size() == 1);
            T.within("progress delivered", 2000, () -> rec.progress.size() == 1);
            /* The ack callback ran on the dispatcher before the progress event was delivered. */
            T.ok(rec.order.indexOf("progress:5:10") >= 0 && seen.size() == 1 && f.isDone(), "reply completed before the progress event that followed it was delivered");
            core.afterReply = null;

            /* Coalescing: 1000 progress frames while the dispatcher is stuck → one delivery with the newest value. */
            rec.progress.clear();
            rec.order.clear();
            rec.gate = new CountDownLatch(1);
            core.injectFrame(FakeCore.RSP_STATUS, 1, 99, eventStatusFrameFor(99));
            T.within("dispatcher blocked again", 2000, () -> rec.status.size() == 2);
            for (int i = 1; i <= 1000; i++) core.emitProgress(42, i, 1000, i);
            core.emitProgress(43, 7, 10, 1);
            core.emitComplete(42, 1000);
            T.sleep(300);
            rec.gate.countDown();
            T.within("complete delivered after the flood", 3000, () -> rec.complete.size() == 1);
            long n42 = rec.progress.stream().filter(x -> x.id() == 42).count();
            T.ok(n42 >= 1 && n42 <= 3, "1000 queued progress events for one download collapsed to " + n42 + " delivery/ies");
            ProgressInfo last42 = rec.progress.stream().filter(x -> x.id() == 42).reduce((a, b) -> b).orElse(null);
            T.eq(1000L, last42 == null ? -1L : last42.downloadedBytes(), "the newest value is the one delivered");
            T.eq(1L, rec.progress.stream().filter(x -> x.id() == 43).count(), "another download's progress is not collapsed into it");
            T.ok(rec.order.indexOf("complete:42") > rec.order.indexOf("progress:42:1000"), "completion is delivered after the final progress, never dropped");
            c.close();
        }
    }

    static void concurrentCommands() throws Exception {
        T.section("Many threads, same download id, mixed commands: every future gets ITS reply");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            core.put(100, FakeCore.DOWNLOADING, 1000, 10);

            final int THREADS = 8, LOOPS = 60;
            AtomicInteger wrong = new AtomicInteger(), total = new AtomicInteger();
            CountDownLatch go = new CountDownLatch(1), done = new CountDownLatch(THREADS);
            for (int t = 0; t < THREADS; t++) {
                new Thread(() -> {
                    try {
                        go.await();
                        for (int i = 0; i < LOOPS; i++) {
                            List<CompletableFuture<AckResult>> fs = new ArrayList<>();
                            fs.add(c.pause(100));                  /* OK whatever the interleaving */
                            fs.add(c.resume(100));                 /* OK */
                            fs.add(c.requestStatus(100));          /* one status row */
                            fs.add(c.setLimit(100, i));            /* OK */
                            fs.add(c.requestStatus(404));          /* NOT_FOUND */
                            fs.add(c.setLimit(404, 1));            /* NOT_FOUND */
                            fs.add(c.pause(404));                  /* NOT_FOUND */
                            boolean[] expectOk = { true, true, true, true, false, false, false };
                            for (int k = 0; k < fs.size(); k++) {
                                AckResult r = fs.get(k).get(10, TimeUnit.SECONDS);
                                total.incrementAndGet();
                                boolean right = expectOk[k] ? r.ok() : r.isNotFound();
                                if (k == 2 && !(r.ok() && r.statuses().size() == 1 && r.statuses().get(0).id() == 100)) right = false;
                                if (!right) wrong.incrementAndGet();
                            }
                        }
                    } catch (Exception e) {
                        wrong.incrementAndGet();
                    } finally { done.countDown(); }
                }).start();
            }
            go.countDown();
            T.ok(done.await(60, TimeUnit.SECONDS), "all threads finished");
            T.eq(THREADS * LOOPS * 7, total.get(), "every command got an answer");
            T.eq(0, wrong.get(), "no command received another command's reply");
            T.ok(c.isConnected(), "the connection stayed healthy throughout");
            T.eq(0, rec.lost.get(), "no connection loss was reported");
            c.close();
        }
    }

    static void statusCommands() throws Exception {
        T.section("STATUS and STATUS_ALL");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            core.put(1, FakeCore.DOWNLOADING, 1000, 100);
            core.put(2, FakeCore.PAUSED, 2000, 200);
            core.put(3, FakeCore.COMPLETE, 3000, 3000);

            AckResult one = get(c.requestStatus(2));
            T.ok(one.ok() && one.statuses().size() == 1, "STATUS returns one row");
            StatusInfo s = one.statuses().get(0);
            T.ok(s.id() == 2 && s.status() == CoreIPCClient.CORE_PAUSED && s.totalBytes() == 2000 && s.downloadedBytes() == 200
                 && s.totalSegments() == 4, "row fields: " + s);
            T.within("the row is also pushed to listeners", 2000, () -> rec.status.size() == 1);

            rec.status.clear();
            AckResult all = get(c.requestStatusAll());
            T.ok(all.ok(), "STATUS_ALL completes on the terminating OK");
            T.eq(3, all.statuses().size(), "...with all three rows");
            T.within("all rows are pushed to listeners as well", 2000, () -> rec.status.size() == 3);
            T.ok(get(c.requestStatus(404)).isNotFound(), "STATUS of an unknown id is NOT_FOUND");

            c.close();
        }
        /* STATUS_ALL on an empty core */
        Path p2 = sock();
        try (FakeCore core = new FakeCore(p2)) {
            CoreIPCClient c = new CoreIPCClient(p2.toString());
            c.connect();
            T.within("connected", 2000, () -> core.hasClient());
            AckResult all = get(c.requestStatusAll());
            T.ok(all.ok() && all.statuses().isEmpty(), "STATUS_ALL with nothing running: OK and zero rows");
            c.close();
        }
    }

    static void connectionLossAndReconnect() throws Exception {
        T.section("Losing the connection, and coming back");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            core.put(1, FakeCore.DOWNLOADING, 100, 0);

            core.mute(true);                                           /* commands are read, never answered */
            CompletableFuture<AckResult> a = c.pause(1), b = c.resume(1), d = c.requestStatusAll();
            T.within("the commands reached the core", 2000, () -> core.commands.size() >= 3);
            long t0 = System.nanoTime();
            core.dropClient();
            AckResult ra = get(a), rb = get(b), rd = get(d);
            long ms = (System.nanoTime() - t0) / 1_000_000;
            T.ok(ra.coreUnreachable() && rb.coreUnreachable() && rd.coreUnreachable(), "all three pending commands fail with ERR_CONNECTION_LOST");
            T.ok(ms < 1500, "...promptly (" + ms + " ms), not after the reply timeout");
            T.within("onConnectionLost fires exactly once", 2000, () -> rec.lost.get() == 1);
            T.ok(!c.isConnected(), "isConnected() is false");
            AckResult nc = get(c.pause(1));
            T.eq(CoreIPCClient.ERR_NOT_CONNECTED, nc.errorCode(), "later commands fail with ERR_NOT_CONNECTED");

            core.mute(false);
            c.connect();
            T.ok(c.isConnected(), "reconnect works");
            T.within("onConnected fires again", 2000, () -> rec.connected.get() == 2);
            T.ok(get(c.pause(1)).ok(), "commands work on the new connection");

            /* A late cleanup of the OLD link must not touch the new one. */
            T.sleep(200);
            T.ok(c.isConnected() && get(c.resume(1)).ok(), "the new connection is unaffected by the old one's teardown");
            T.eq(1, rec.lost.get(), "still only one loss reported");

            /* The core process dies and a fresh one comes up. */
            core.kill();
            T.within("loss detected when the core dies", 2000, () -> rec.lost.get() == 2);
            boolean refused = false;
            try { c.connect(); } catch (java.io.IOException e) { refused = true; }
            T.ok(refused, "connect() to a dead socket throws IOException");
            core.restart();
            c.connect();
            T.ok(get(c.requestStatusAll()).statuses().isEmpty(), "a fresh core knows no downloads");
            c.close();
        }
    }

    static void timeouts() throws Exception {
        T.section("A command that gets no reply in time: NO_REPLY, and the link is dropped");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            c.setAckTimeoutMillis(300);
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();
            core.put(1, FakeCore.DOWNLOADING, 100, 0);

            core.mute(true);
            long t0 = System.nanoTime();
            CompletableFuture<AckResult> f = c.pause(1);
            CompletableFuture<AckResult> g = c.resume(1);              /* queued behind it */
            AckResult r = get(f);
            long ms = (System.nanoTime() - t0) / 1_000_000;
            T.ok(r.noReply() && r.clientSide(), "the first command completes with ERR_NO_REPLY");
            T.ok(ms >= 250 && ms < 2000, "...after about the timeout (" + ms + " ms)");
            AckResult rg = get(g);
            T.ok(rg.coreUnreachable() || rg.noReply(), "the command queued behind it fails too (" + rg.errorCode() + ")");
            T.within("the connection was dropped: the reply order can no longer be trusted", 2000, () -> !c.isConnected());
            T.within("...and listeners were told", 2000, () -> rec.lost.get() == 1);

            core.mute(false);
            c.connect();
            T.ok(get(c.pause(1)).ok(), "after reconnecting, commands are answered correctly again");

            /* A slow but alive core: reply later than the timeout. */
            core.replyDelay(600);
            AckResult slow = get(c.pause(1));
            T.ok(slow.noReply(), "a reply slower than the timeout is reported as NO_REPLY");
            core.replyDelay(0);
            c.close();
        }
    }

    /** Connects and waits until the fake core has accepted THAT connection, so whatever the test
     *  injects next goes down the new link and not down the one that was just dropped. */
    static void reconnect(CoreIPCClient c, FakeCore core) throws Exception {
        int seen = core.accepts();
        c.connect();
        if (!core.awaitAccepts(seen + 1, 3000)) throw new IllegalStateException("the fake core never accepted the new connection");
    }

    static void protocolViolations() throws Exception {
        T.section("Protocol violations drop the connection instead of corrupting state");
        byte[] badMagic = new byte[16];
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec rec = new Rec();
            c.addListener(rec);
            c.connect();

            core.injectFrame(FakeCore.RSP_OK, 0, 5, null);
            T.sleep(150);
            T.ok(c.isConnected() && rec.lost.get() == 0, "an unsolicited OK is ignored");
            core.put(1, FakeCore.DOWNLOADING, 10, 0);
            T.ok(get(c.pause(1)).ok(), "...and commands still work afterwards");

            core.replyDelay(200);
            CompletableFuture<AckResult> f = c.pause(1);               /* a SIMPLE command is pending ... */
            T.sleep(50);
            core.injectFrame(FakeCore.RSP_STATUS, 0, 1, eventStatusFrameFor(1));   /* ... and a STATUS reply (flags 0) arrives */
            AckResult r = get(f);
            T.ok(!r.ok() && r.coreUnreachable(), "a reply of the wrong kind fails the pending command and drops the link");
            T.within("onConnectionLost reports it", 2000, () -> rec.lost.get() == 1);
            T.ok(rec.lostReason != null && rec.lostReason.contains("RSP_STATUS"), "...with a reason (" + rec.lostReason + ")");
            core.replyDelay(0);

            reconnect(c, core);
            core.injectRaw(badMagic);
            T.within("bad magic drops the connection", 2000, () -> rec.lost.get() == 2);

            reconnect(c, core);
            ByteBuffer big = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
            big.putInt(FakeCore.MAGIC).put((byte) 1).put((byte) FakeCore.RSP_OK).putShort((short) 0).putInt(1).putInt((1 << 20) + 1);
            core.injectRaw(big.array());
            T.within("an oversized payload length drops the connection", 2000, () -> rec.lost.get() == 3);

            reconnect(c, core);
            ByteBuffer ver = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
            ver.putInt(FakeCore.MAGIC).put((byte) 9).put((byte) FakeCore.RSP_OK).putShort((short) 0).putInt(1).putInt(0);
            core.injectRaw(ver.array());
            T.within("an unknown protocol version drops the connection", 2000, () -> rec.lost.get() == 4);

            reconnect(c, core);
            core.injectFrame(0x7E, 1, 1, new byte[4]);                 /* unknown event type: skipped */
            core.injectFrame(FakeCore.RSP_PROGRESS, 1, 1, new byte[5]);/* short progress payload: skipped */
            T.sleep(150);
            T.ok(c.isConnected() && rec.lost.get() == 4, "unknown frame types and short event payloads are skipped, not fatal");
            T.ok(get(c.pause(1)).ok(), "...and the stream is still in step");
            c.close();
        }
    }

    static void argumentChecks() throws Exception {
        T.section("Bad arguments are rejected before anything is written");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            c.connect();
            int before = core.commands.size();
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(0, "http://h/x", "/tmp/x", 4, 0, 0, null)).errorCode(), "START with id 0");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(-5, "http://h/x", "/tmp/x", 4, 0, 0, null)).errorCode(), "negative id");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(0x1_0000_0000L, "http://h/x", "/tmp/x", 4, 0, 0, null)).errorCode(), "id above 32 bits");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(1, "http://h/" + "a".repeat(4100), "/tmp/x", 4, 0, 0, null)).errorCode(), "URL longer than the core accepts");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(1, "http://h/x", "/" + "b".repeat(1100), 4, 0, 0, null)).errorCode(), "path longer than the core accepts");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.start(1, null, "/tmp/x", 4, 0, 0, null)).errorCode(), "null URL");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.pause(0)).errorCode(), "PAUSE of id 0");
            T.ok(get(c.setLimit(0, 5)).ok(), "SET_LIMIT with id 0 IS valid (the global limit)");
            T.eq(CoreIPCClient.ERR_BAD_ARGUMENT, get(c.setLimit(-1, 5)).errorCode(), "SET_LIMIT with a negative id");
            T.eq(before + 1, core.commands.size(), "nothing but the valid SET_LIMIT reached the core");
            c.close();
        }
    }

    static void listenerIsolationAndClose() throws Exception {
        T.section("A failing listener does not break the others; close() is final");
        Path p = sock();
        try (FakeCore core = new FakeCore(p)) {
            CoreIPCClient c = new CoreIPCClient(p.toString());
            Rec good = new Rec();
            c.addListener(new IPCEventListener() {
                @Override public void onProgress(ProgressInfo i) { throw new IllegalStateException("listener bug (expected in this test)"); }
                @Override public void onComplete(CompleteInfo i) { throw new IllegalStateException("listener bug (expected in this test)"); }
            });
            c.addListener(good);
            c.connect();
            core.put(1, FakeCore.DOWNLOADING, 10, 0);
            core.emitProgress(1, 5, 10, 1);
            core.emitComplete(1, 10);
            T.within("the second listener still gets progress and completion", 2000, () -> good.progress.size() == 1 && good.complete.size() == 1);
            T.ok(get(c.requestStatus(1)).ok(), "commands keep working");

            core.mute(true);
            CompletableFuture<AckResult> f = c.pause(1);
            T.within("pending", 2000, () -> core.commands(FakeCore.PAUSE).size() == 1);
            c.close();
            T.ok(get(f).coreUnreachable(), "close() fails pending commands");
            boolean threw = false;
            try { c.connect(); } catch (IllegalStateException e) { threw = true; }
            T.ok(threw, "connect() after close() is refused");
            T.ok(get(c.pause(1)).coreUnreachable(), "commands after close() fail cleanly");
        }
        Files.deleteIfExists(p);
    }
}
