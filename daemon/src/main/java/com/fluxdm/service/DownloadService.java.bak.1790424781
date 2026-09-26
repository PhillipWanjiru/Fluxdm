/* FluxDM — service/DownloadService.java
 * Bridges REST requests to the C core over CoreIPCClient, and implements
 * IPCEventListener to persist progress/completion/error events pushed
 * back from the core, broadcasting live updates to the UI over WebSocket.
 *
 * Admission is gated by QueueService: only up to
 * fluxdm.max-concurrent-downloads run at once. Anything over that limit
 * is saved to the DB (status PENDING) but not sent to the C core yet —
 * it starts automatically once a running download finishes and frees a
 * slot, via releaseSlotAndAdmitNext() below.
 */
package com.fluxdm.service;

import com.fluxdm.db.DownloadRepository;
import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.IPCEventListener;
import com.fluxdm.model.Download;
import com.fluxdm.model.Stats;

import jakarta.annotation.PostConstruct;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.stereotype.Service;

import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.TimeUnit;

@Service
public class DownloadService implements IPCEventListener {

    /* Status codes — mirrored in db/schema.sql and the React UI */
    private static final int STATUS_PENDING = 0;
    private static final int STATUS_RUNNING = 1;
    private static final int STATUS_PAUSED  = 2;
    private static final int STATUS_DONE    = 3;
    private static final int STATUS_ERROR   = 4;

    @Autowired private DownloadRepository    repository;
    @Autowired private CoreIPCClient         ipcClient;
    @Autowired private QueueService          queueService;
    @Autowired private SimpMessagingTemplate messaging;

    @PostConstruct
    void registerAsListener() {
        ipcClient.addListener(this);
    }

    /* ── REST-facing operations ──────────────────────────────────────────── */

    public List<Download> getAll() { return repository.findAll(); }

    public Optional<Download> findById(long id) { return repository.findById(id); }

    public Download start(Download dl) {
        dl.setStatus(STATUS_PENDING);
        dl.setDownloaded(0L);   /* never trust a client-supplied initial progress value */
        dl.setErrorMessage(null);
        dl = repository.save(dl);   /* assigns the real DB id we send to the core */

        if (queueService.tryAdmit(dl)) {
            actuallyStartDownload(dl);
        }
        /* else: stays PENDING in the DB and sits in QueueService's internal
         * queue until a running slot frees up — see onComplete/onError. */

        return dl;
    }

    /** Sends CMD_START to the C core for a download that's been admitted
     *  (i.e. QueueService already counted it against the concurrency
     *  limit). Never call this directly from start() without going
     *  through tryAdmit() first, or the concurrency limit means nothing. */
    private void actuallyStartDownload(Download dl) {
        byte[] checksumBytes = null;
        if (dl.getChecksum() != null && !dl.getChecksum().isBlank()) {
            try {
                checksumBytes = HexFormat.of().parseHex(dl.getChecksum());
            } catch (IllegalArgumentException e) {
                /* Malformed hex — proceed without checksum verification rather
                 * than fail the whole download over a cosmetic input error. */
                checksumBytes = null;
            }
        }

        int id = dl.getId().intValue();
        int checksumType = dl.getChecksumType() != null ? dl.getChecksumType() : 0;

        var ack = ipcClient.start(id, dl.getUrl(), dl.getFilePath(),
                                  dl.getSegments(), 0L, checksumType, checksumBytes);

        long dlId = dl.getId();
        ack.orTimeout(15, TimeUnit.SECONDS).whenComplete((result, ex) -> {
            if (ex != null || result == null || !result.ok()) {
                String reason = ex != null ? ex.getMessage()
                               : (result != null ? result.errorMessage() : "no response");
                markError(dlId, reason);   /* also releases the slot and admits next */
            } else {
                markStatus(dlId, STATUS_RUNNING);
            }
        });
    }

    public void pause(long id) {
        repository.findById(id).ifPresent(dl -> {
            ipcClient.pause(dl.getId().intValue());
            markStatus(id, STATUS_PAUSED);
        });
    }

    public void resume(long id) {
        repository.findById(id).ifPresent(dl -> {
            ipcClient.resume(dl.getId().intValue());
            markStatus(id, STATUS_RUNNING);
        });
    }

    public void cancel(long id) {
        repository.findById(id).ifPresent(dl -> {
            boolean wasQueued = queueService.removeFromQueue(dl);
            if (!wasQueued) {
                /* Only tell the core to cancel if it actually knows about
                 * this download — a still-queued item was never sent via
                 * CMD_START, so the core has no record of it at all. */
                ipcClient.cancel(dl.getId().intValue());
                admitNextQueued();   /* this one held a concurrency slot — free it */
            }
            repository.deleteById(id);
            broadcast(id, Map.of("type", "cancelled", "id", id));
        });
    }

    public Stats getStats() {
        Stats s = new Stats();
        s.setTotal(repository.count());
        s.setActive(repository.countByStatus(STATUS_RUNNING));
        s.setComplete(repository.countByStatus(STATUS_DONE));
        return s;
    }

    /* ── IPCEventListener — pushed asynchronously from the core ────────────── */

    @Override
    public void onProgress(CoreIPCClient.ProgressInfo info) {
        long id = info.id();
        repository.findById(id).ifPresent(dl -> {
            dl.setTotalSize(info.totalBytes());
            dl.setDownloaded(info.downloadedBytes());
            dl.setStatus(STATUS_RUNNING);
            repository.save(dl);
        });

        broadcast(id, Map.of(
            "type",             "progress",
            "id",                id,
            "totalBytes",        info.totalBytes(),
            "downloadedBytes",   info.downloadedBytes(),
            "speedBps",          info.speedBps(),
            "etaSeconds",        info.etaSeconds(),
            "activeSegments",    info.activeSegments()
        ));
    }

    @Override
    public void onComplete(CoreIPCClient.CompleteInfo info) {
        long id = info.id();
        repository.findById(id).ifPresent(dl -> {
            dl.setTotalSize(info.totalBytes());
            dl.setDownloaded(info.totalBytes());
            dl.setStatus(STATUS_DONE);
            repository.save(dl);
        });

        broadcast(id, Map.of(
            "type",         "complete",
            "id",            id,
            "totalBytes",    info.totalBytes(),
            "elapsedMs",     info.elapsedMs(),
            "checksumOk",    info.checksumOk()
        ));

        admitNextQueued();
    }

    @Override
    public void onError(int downloadId, int errorCode, String errorMessage) {
        markError(downloadId, errorMessage);
    }

    @Override
    public void onStatus(CoreIPCClient.StatusInfo info) {
        broadcast(info.id(), Map.of(
            "type",              "status",
            "id",                 info.id(),
            "status",             info.status(),
            "activeSegments",     info.activeSegments(),
            "totalSegments",      info.totalSegments(),
            "totalBytes",         info.totalBytes(),
            "downloadedBytes",    info.downloadedBytes(),
            "speedBps",           info.speedBps(),
            "etaSeconds",         info.etaSeconds()
        ));
    }

    /* ── Internal helpers ─────────────────────────────────────────────────── */

    private void markStatus(long id, int status) {
        repository.findById(id).ifPresent(dl -> {
            dl.setStatus(status);
            repository.save(dl);
        });
    }

    private void markError(long id, String message) {
        String reason = message != null ? message : "unknown error";
        System.err.println("[fluxdm-daemon] download " + id + " failed: " + reason);
        repository.findById(id).ifPresent(dl -> {
            dl.setStatus(STATUS_ERROR);
            dl.setErrorMessage(reason);
            repository.save(dl);
        });
        broadcast(id, Map.of("type", "error", "id", id, "message", reason));
        admitNextQueued();
    }

    /** Frees the concurrency slot held by whatever just finished (done,
     *  errored, or was cancelled while running) and, if anything is
     *  waiting, starts it immediately. */
    private void admitNextQueued() {
        Download next = queueService.releaseSlotAndAdmitNext();
        if (next != null) actuallyStartDownload(next);
    }

    private void broadcast(long downloadId, Object payload) {
        /* Per-download topic so the UI can subscribe to just what it's showing,
         * plus a global topic for the download list view. */
        messaging.convertAndSend("/topic/downloads/" + downloadId, payload);
        messaging.convertAndSend("/topic/downloads", payload);
    }
}
