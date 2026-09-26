/* Standalone protocol test for CoreIPCClient — connects to mock_core.py,
 * which speaks the exact Phase-1-validated fluxdm-core wire protocol.
 * No JUnit/Spring needed — self-contained, mirrors the C/C++ test style. */
package com.fluxdm.test;

import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.IPCEventListener;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.List;
import java.util.Collections;
import java.util.ArrayList;

public class TestCoreIPCClient {

    static int pass = 0, fail = 0;

    static void expect(boolean cond, String desc) {
        if (cond) { System.out.println("  \u2714  " + desc); pass++; }
        else      { System.out.println("  \u2718  " + desc); fail++; }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM Phase 3 — CoreIPCClient wire protocol test\n");

        CoreIPCClient client = new CoreIPCClient("/tmp/fluxdm_mock.sock");

        List<CoreIPCClient.ProgressInfo> progressEvents =
            Collections.synchronizedList(new ArrayList<>());
        CountDownLatch completeLatch = new CountDownLatch(1);
        AtomicInteger completeId = new AtomicInteger(-1);

        client.addListener(new IPCEventListener() {
            @Override public void onProgress(CoreIPCClient.ProgressInfo info) {
                progressEvents.add(info);
            }
            @Override public void onComplete(CoreIPCClient.CompleteInfo info) {
                completeId.set(info.id());
                completeLatch.countDown();
            }
        });

        System.out.println("── Connecting");
        client.connect();
        expect(client.isConnected(), "client.isConnected()");

        // Give the mock server a moment to accept()
        Thread.sleep(100);

        System.out.println("\n── CMD_START round trip");
        CompletableFuture<CoreIPCClient.AckResult> startFuture =
            client.start(42, "https://example.com/file.bin", "/tmp/out.bin",
                        4, 0L, 0, null);

        CoreIPCClient.AckResult startResult = startFuture.get(5, TimeUnit.SECONDS);
        expect(startResult.ok(), "RSP_OK for CMD_START");

        System.out.println("\n── Waiting for RSP_PROGRESS x3 + RSP_COMPLETE");
        boolean completed = completeLatch.await(5, TimeUnit.SECONDS);
        expect(completed, "RSP_COMPLETE received within timeout");
        expect(completeId.get() == 42, "complete event id matches (got " + completeId.get() + ")");
        expect(progressEvents.size() == 3, "received 3 progress events (got " + progressEvents.size() + ")");

        if (!progressEvents.isEmpty()) {
            var p0 = progressEvents.get(0);
            expect(p0.id() == 42, "progress[0].id == 42");
            expect(p0.totalBytes() == 1048576, "progress[0].totalBytes == 1048576");
            expect(p0.downloadedBytes() == 1048576 / 4, "progress[0].downloadedBytes == total/4");
            expect(p0.speedBps() == 2_000_000, "progress[0].speedBps == 2000000");
            expect(p0.activeSegments() == 4, "progress[0].activeSegments == 4");

            var p2 = progressEvents.get(2);
            expect(p2.downloadedBytes() == 1048576 * 3 / 4, "progress[2].downloadedBytes == total*3/4");
        }

        System.out.println("\n── CMD_PAUSE / CMD_RESUME / CMD_CANCEL");
        expect(client.pause(42).get(5, TimeUnit.SECONDS).ok(), "RSP_OK for CMD_PAUSE");
        expect(client.resume(42).get(5, TimeUnit.SECONDS).ok(), "RSP_OK for CMD_RESUME");
        expect(client.cancel(42).get(5, TimeUnit.SECONDS).ok(), "RSP_OK for CMD_CANCEL");

        System.out.println("\n── CMD_SET_LIMIT");
        expect(client.setLimit(0, 512_000).get(5, TimeUnit.SECONDS).ok(),
               "RSP_OK for CMD_SET_LIMIT");

        System.out.println("\n── CMD_STATUS");
        CountDownLatch statusLatch = new CountDownLatch(1);
        AtomicInteger statusVal = new AtomicInteger(-1);
        client.addListener(new IPCEventListener() {
            @Override public void onStatus(CoreIPCClient.StatusInfo info) {
                statusVal.set(info.status());
                statusLatch.countDown();
            }
        });
        client.requestStatus(42);
        expect(statusLatch.await(5, TimeUnit.SECONDS), "RSP_STATUS received");
        expect(statusVal.get() == 2, "status field == 2 (DOWNLOADING), got " + statusVal.get());

        System.out.println("\n── Error path: segments=0 should trigger RSP_ERROR");
        CompletableFuture<CoreIPCClient.AckResult> errFuture =
            client.start(99, "https://bad.example.com/x", "/tmp/x.bin", 0, 0, 0, null);
        CoreIPCClient.AckResult errResult = errFuture.get(5, TimeUnit.SECONDS);
        expect(!errResult.ok(), "RSP_ERROR (not ok) for segments=0");
        expect(errResult.errorMessage() != null &&
               errResult.errorMessage().contains("invalid segment count"),
               "error message contains expected text (got: '" + errResult.errorMessage() + "')");

        System.out.println("\n── Shutdown");
        client.shutdownCore();
        Thread.sleep(300);

        client.disconnect();

        System.out.println("\n─────────────────────────────");
        System.out.println("  Results");
        System.out.println("─────────────────────────────");
        System.out.println("  Passed: " + pass);
        System.out.println("  Failed: " + fail);
        System.out.println("─────────────────────────────\n");

        if (fail == 0) System.out.println("ALL TESTS PASSED\n");
        else           System.out.println("FAILED\n");

        System.exit(fail > 0 ? 1 : 0);
    }
}
