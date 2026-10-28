/* FluxDM — service/DownloadService.java
 * The daemon's download logic: REST requests in, commands to fluxdm-core out,
 * events from the core back into the database and out to the UI over STOMP.
 *
 * How the pieces fit
 *
 *   QueueService   decides WHO may run (concurrency slots, by download id).
 *   CoreIPCClient  talks to the core; every command answers with an AckResult.
 *   this class     keeps the database, the slots and the core in step.
 *
 * The rules that keep them in step
 *
 *   1. One lock (stateLock) guards every database read-modify-write together
 *      with the slot bookkeeping it belongs to. It is NEVER held while talking
 *      to the core: commands are sent after it is released and their results
 *      are applied under it again, re-checking the state they find. Whatever
 *      the user did in the meantime wins; the result only fills in what is
 *      still undecided.
 *
 *   2. A download holds a slot exactly while it is "in flight in the core":
 *      from the moment it is admitted until it completes, fails, is paused or
 *      is cancelled. Slots are released by id and idempotently, so two signals
 *      for the same end (an error event racing a cancel) free one slot, not two.
 *
 *   3. Status changes are guarded transitions (PENDING -> RUNNING only from
 *      PENDING, ERROR only from PENDING/RUNNING, ...). A late event for a
 *      download the user already paused or cancelled cannot undo that.
 *
 *   4. Pause/resume/cancel wait for the core's answer (bounded), so the HTTP
 *      response tells the truth and the status is only changed when the core
 *      agreed. A user command on a download whose START/RESUME is still in
 *      flight waits for that to settle first, so the commands reach the core
 *      in the order the user gave them.
 *
 *   5. After every (re)connection the daemon asks the core what it is running
 *      and reconciles it with the database (reconcile()): downloads that were
 *      running when the daemon or the core went down are started again and
 *      carry on from their .fluxpart checkpoint; downloads the core finished
 *      or lost in the meantime are corrected. Until that has happened the
 *      operations that need the core answer 503 instead of guessing.
 *
 * Listener callbacks (onProgress, onComplete, ...) run on the IPC client's
 * dispatcher thread and never wait on a command future.
 */
package com.fluxdm.service;

import com.fluxdm.db.DownloadRepository;
import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.CoreIPCClient.AckResult;
import com.fluxdm.ipc.CoreIPCClient.CompleteInfo;
import com.fluxdm.ipc.CoreIPCClient.ProgressInfo;
import com.fluxdm.ipc.CoreIPCClient.StatusInfo;
import com.fluxdm.ipc.IPCEventListener;
import com.fluxdm.model.Download;
import com.fluxdm.model.Stats;
import com.fluxdm.service.QueueService.Admission;
import com.fluxdm.service.QueueService.Mode;
import com.fluxdm.service.QueueService.Outcome;

import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.messaging.simp.SimpMessagingTemplate;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.regex.Pattern;

@Service
public class DownloadService implements IPCEventListener {

    private static final Logger log = LoggerFactory.getLogger(DownloadService.class);

    /* Status codes — mirrored in db/schema.sql and the React UI */
    public static final int STATUS_PENDING = 0;
    public static final int STATUS_RUNNING = 1;
    public static final int STATUS_PAUSED  = 2;
    public static final int STATUS_DONE    = 3;
    public static final int STATUS_ERROR   = 4;

    private static final List<Integer> OPEN_STATES       = List.of(STATUS_PENDING, STATUS_RUNNING, STATUS_PAUSED);
    private static final List<Integer> RECONCILED_STATES = List.of(STATUS_PENDING, STATUS_RUNNING, STATUS_PAUSED, STATUS_ERROR);

    private static final int  DEFAULT_SEGMENTS   = 8;
    private static final int  MAX_SEGMENTS       = 64;
    private static final int  MAX_URL_BYTES      = 4095;     /* what the core accepts */
    private static final int  MAX_PATH_BYTES     = 1023;
    private static final int  MAX_ERROR_CHARS    = 500;
    private static final long PERSIST_EVERY_MS   = 2_000;    /* progress is written to the DB at most this often */
    private static final long WAIT_FOR_CORE_SECS = CoreIPCClient.ACK_TIMEOUT_SECS + 5;

    private static final Pattern URL_SHAPE = Pattern.compile("^https?://[^\\s/?#]+(?:[/?#]\\S*)?$",
                                                             Pattern.CASE_INSENSITIVE);
    private static final Pattern HEX       = Pattern.compile("^[0-9a-fA-F]+$");

    private final DownloadRepository    repository;
    private final CoreIPCClient         ipc;
    private final QueueService          queue;
    private final SimpMessagingTemplate messaging;
    private final boolean               autoResume;

    private final Object stateLock = new Object();

    /** The core's state has been reconciled with the database since the last (re)connect. Written under stateLock. */
    private volatile boolean synced;

    /** Downloads whose START/RESUME has been sent and not yet answered. */
    private final ConcurrentHashMap<Long, CompletableFuture<Void>> launching = new ConcurrentHashMap<>();

    /** Live numbers of downloads the core is working on. */
    private final ConcurrentHashMap<Long, Live> live = new ConcurrentHashMap<>();

    private static final class Live {
        volatile long    total, downloaded, speed;
        volatile long    lastPersistMs;
        volatile boolean promoted;
    }

    /** Everything a START needs, copied out of the database row. */
    private record Spec(long id, String url, String path, int segments, long limit,
                        int checksumType, byte[] checksum) {
        static Spec of(Download d) {
            byte[] cksum = null;
            int type = d.getChecksumType() != null ? d.getChecksumType() : 0;
            if (type != 0 && d.getChecksum() != null && !d.getChecksum().isBlank()) {
                try {
                    cksum = HexFormat.of().parseHex(d.getChecksum());
                } catch (IllegalArgumentException e) {
                    type = 0;      /* validated on the way in; a hand-edited row must not take the download down */
                }
            } else {
                type = 0;
            }
            return new Spec(d.getId(), d.getUrl(), d.getFilePath(),
                            d.getSegments() != null ? d.getSegments() : DEFAULT_SEGMENTS,
                            d.getBandwidthLimit(), type, cksum);
        }
    }

    public DownloadService(DownloadRepository repository,
                           CoreIPCClient ipc,
                           QueueService queue,
                           SimpMessagingTemplate messaging,
                           @Value("${fluxdm.recovery.auto-resume:true}") boolean autoResume) {
        this.repository = repository;
        this.ipc        = ipc;
        this.queue      = queue;
        this.messaging  = messaging;
        this.autoResume = autoResume;
    }

    @PostConstruct
    void registerAsListener() {
        ipc.addListener(this);
    }

    /* ═══ REST-facing operations ═══════════════════════════════════════════ */

    public List<Download> getAll() {
        List<Download> all = repository.findAll();
        for (Download d : all) decorate(d);
        return all;
    }

    public Optional<Download> findById(long id) {
        return repository.findById(id).map(this::decorate);
    }

    /** True once the core's downloads have been reconciled with the database. */
    public boolean isSynced() { return synced; }

    /** Validates and stores a new download and starts it, or queues it if every slot is taken. */
    public Download start(Download request) {
        Download fresh = validated(request);

        Download saved;
        Outcome  outcome;
        synchronized (stateLock) {
            requireSynced();
            if (repository.existsByFilePathAndStatusIn(fresh.getFilePath(), OPEN_STATES))
                throw DownloadException.conflict("another download is already using " + fresh.getFilePath());
            saved   = repository.save(fresh);                       /* assigns the id the core will know it by */
            outcome = queue.submit(saved.getId(), saved.getPriority(), Mode.START);
        }

        long id = saved.getId();
        if (outcome == Outcome.ADMITTED) awaitQuietly(launch(id, Mode.START));
        /* else: stays PENDING and starts when a slot frees up — see release paths below */

        return repository.findById(id).map(this::decorate).orElse(saved);
    }

    public Download pause(long id) {
        awaitLaunch(id);

        boolean neverStarted = false;
        synchronized (stateLock) {
            Download dl = require(id);
            switch (dl.getStatus()) {
                case STATUS_PAUSED -> { return decorate(dl); }
                case STATUS_DONE, STATUS_ERROR ->
                    throw DownloadException.conflict("download " + id + " is " + statusName(dl.getStatus())
                                                     + " and cannot be paused");
                default -> { /* pending or running: carry on */ }
            }
            if (queue.isWaiting(id)) {
                queue.release(id);                                   /* it never reached the core */
                dl.setStatus(STATUS_PAUSED);
                repository.save(dl);
                neverStarted = true;
            } else {
                requireSynced();
            }
        }
        if (neverStarted) {
            broadcastState(id, STATUS_PAUSED, null);
            return require(id);
        }

        AckResult res = await(ipc.pause(id));

        List<Admission> follow;
        synchronized (stateLock) {
            Download dl = repository.findById(id).orElse(null);
            if (dl == null) throw DownloadException.notFound(id);

            if (res.ok() || res.isNotFound()) {                      /* "not found": the core is not running it; paused is the truth */
                if (dl.getStatus() == STATUS_PENDING || dl.getStatus() == STATUS_RUNNING) {
                    dl.setStatus(STATUS_PAUSED);
                    repository.save(dl);
                }
                live.remove(id);
                follow = queue.release(id);
            } else if (res.coreUnreachable() || res.noReply()) {
                throw DownloadException.unavailable(describe(res));
            } else {
                throw DownloadException.conflict(describe(res));
            }
        }
        broadcastState(id, STATUS_PAUSED, null);
        launchAll(follow);
        return require(id);
    }

    public Download resume(long id) {
        awaitLaunch(id);

        Outcome outcome;
        synchronized (stateLock) {
            Download dl = require(id);
            switch (dl.getStatus()) {
                case STATUS_PENDING, STATUS_RUNNING -> { return decorate(dl); }   /* already going, or waiting its turn */
                case STATUS_DONE -> throw DownloadException.conflict("download " + id + " is already complete");
                default -> { /* paused or failed */ }
            }
            requireSynced();
            dl.setStatus(STATUS_PENDING);
            dl.setErrorMessage(null);
            repository.save(dl);
            outcome = queue.submit(id, dl.getPriority(), Mode.RESUME);
        }
        broadcastState(id, STATUS_PENDING, null);

        if (outcome == Outcome.ADMITTED) awaitQuietly(launch(id, Mode.RESUME));
        return require(id);
    }

    /** Stops the download, deletes its partial data and removes it from the list.
     *  A finished download's file is never touched. Cancelling something that is not there is not an error. */
    public void cancel(long id) {
        awaitLaunch(id);

        Download dl;
        boolean  needsCore;
        synchronized (stateLock) {
            dl = repository.findById(id).orElse(null);
            if (dl == null) return;
            needsCore = dl.getStatus() != STATUS_DONE && !queue.isWaiting(id);
            if (needsCore) requireSynced();
        }

        if (needsCore) {
            AckResult res = await(ipc.cancel(id));                   /* the core answers OK even for ids it has forgotten */
            if (!res.ok()) throw DownloadException.unavailable(describe(res));
        }

        List<Admission> follow;
        synchronized (stateLock) {
            repository.deleteById(id);
            live.remove(id);
            follow = queue.release(id);
        }
        if (dl.getStatus() != STATUS_DONE) removeLeftovers(dl);      /* the core forgot it (restart, retention expiry): clean up here */
        broadcast(id, Map.of("type", "cancelled", "id", id));
        launchAll(follow);
    }

    /** Sets (bytes per second, 0 = unlimited) the speed limit of one download. It is remembered
     *  and applied again whenever the download is started. */
    public Download setLimit(long id, long bytesPerSec) {
        if (bytesPerSec < 0) throw DownloadException.badRequest("the limit cannot be negative");
        synchronized (stateLock) {
            Download dl = require(id);
            dl.setBandwidthLimit(bytesPerSec);
            repository.save(dl);
        }
        AckResult res = await(ipc.setLimit(id, bytesPerSec));
        /* NOT_FOUND: the core is not running it right now — the stored value is used at the next START.
         * Unreachable: same. Anything else is the core refusing the value. */
        if (!res.ok() && !res.isNotFound() && !res.coreUnreachable())
            throw DownloadException.conflict(describe(res));
        return require(id);
    }

    public Stats getStats() {
        Stats s = new Stats();
        s.setTotal(repository.count());
        s.setActive(repository.countByStatus(STATUS_RUNNING));
        s.setComplete(repository.countByStatus(STATUS_DONE));
        s.setQueued(queue.queuedCount());
        long speed = 0;
        for (Live l : live.values()) speed += l.speed;
        s.setSpeedBps(speed);
        return s;
    }

    /* ═══ Launching ═════════════════════════════════════════════════════════ */

    /** Sends START (or RESUME) for a download that holds a slot. Never blocks; the returned
     *  future completes once the core's answer has been applied. Safe from any thread. */
    private CompletableFuture<Void> launch(long id, Mode mode) {
        Spec spec = null;
        List<Admission> follow = List.of();
        synchronized (stateLock) {
            Download dl = repository.findById(id).orElse(null);
            if (dl != null && dl.getStatus() == STATUS_PENDING) spec = Spec.of(dl);
            else follow = queue.release(id);          /* deleted or paused while it waited: pass the slot on */
        }
        if (spec == null) {
            launchAll(follow);
            return CompletableFuture.completedFuture(null);
        }

        CompletableFuture<AckResult> ack = (mode == Mode.RESUME)
            ? ipc.resume(id)
            : ipc.start(id, spec.url(), spec.path(), spec.segments(), spec.limit(),
                        spec.checksumType(), spec.checksum());

        CompletableFuture<Void> done = ack
            .handle((res, ex) -> res != null ? res
                                             : AckResult.failure(CoreIPCClient.ERR_PROTOCOL, String.valueOf(ex)))
            .thenCompose(res -> onLaunchResult(id, mode, res))
            .exceptionally(t -> {
                log.error("applying the core's reply for download {} failed", id, t);
                return null;
            });
        launching.put(id, done);
        done.whenComplete((v, t) -> launching.remove(id, done));
        return done;
    }

    private void launchAll(List<Admission> admissions) {
        for (Admission a : admissions) launch(a.id(), a.mode());
    }

    private CompletableFuture<Void> onLaunchResult(long id, Mode mode, AckResult res) {
        if (res.ok()) {
            afterLaunchAccepted(id);
            return CompletableFuture.completedFuture(null);
        }
        if (mode == Mode.RESUME && res.isNotFound()) {
            /* The core has forgotten the download (restart, retention expiry): start it afresh,
             * it picks its partial file up again from the .fluxpart checkpoint. */
            return launch(id, Mode.START);
        }
        if (res.coreUnreachable()) {
            /* Stays PENDING and keeps its slot: when the connection comes back, reconcile()
             * starts it again. */
            log.warn("download {} could not be sent to fluxdm-core: {}", id, res.errorMessage());
            return CompletableFuture.completedFuture(null);
        }
        if (res.noReply()) ipc.cancel(id);            /* may or may not have been applied: make sure it is not left running */
        failDownload(id, describe(res));
        return CompletableFuture.completedFuture(null);
    }

    private void afterLaunchAccepted(long id) {
        boolean running = false, pauseIt = false, cancelIt = false;
        synchronized (stateLock) {
            Download dl = repository.findById(id).orElse(null);
            if (dl == null) {
                cancelIt = true;                      /* cancelled while the command was in flight */
            } else {
                switch (dl.getStatus()) {
                    case STATUS_PENDING -> {
                        dl.setStatus(STATUS_RUNNING);
                        dl.setErrorMessage(null);
                        repository.save(dl);
                        running = true;
                    }
                    case STATUS_PAUSED -> pauseIt  = true;   /* paused before the core had taken it */
                    case STATUS_ERROR  -> cancelIt = true;   /* failed on our side, accepted by the core after all */
                    default -> { /* running already (a progress event beat the reply) or done */ }
                }
            }
        }
        if (running) broadcastState(id, STATUS_RUNNING, null);
        if (cancelIt) ipc.cancel(id);
        if (pauseIt)  ipc.pause(id);
    }

    /** Marks a pending or running download as failed and passes its slot on. */
    private void failDownload(long id, String message) {
        String reason = truncate(message != null && !message.isBlank() ? message : "unknown error");
        boolean changed = false;
        List<Admission> follow;
        synchronized (stateLock) {
            Download dl = repository.findById(id).orElse(null);
            if (dl != null && (dl.getStatus() == STATUS_PENDING || dl.getStatus() == STATUS_RUNNING)) {
                dl.setStatus(STATUS_ERROR);
                dl.setErrorMessage(reason);
                repository.save(dl);
                changed = true;
            }
            live.remove(id);
            follow = queue.release(id);
        }
        if (changed) {
            log.warn("download {} failed: {}", id, reason);
            broadcast(id, Map.of("type", "error", "id", id, "message", reason));
            broadcastState(id, STATUS_ERROR, reason);
        }
        launchAll(follow);
    }

    /* ═══ IPCEventListener — pushed asynchronously from the core ════════════ */

    @Override
    public void onProgress(ProgressInfo info) {
        long id = info.id();
        if (!queue.isActive(id)) return;              /* late event for something paused, cancelled or finished */

        Live lv = live.computeIfAbsent(id, k -> new Live());
        lv.total      = info.totalBytes();
        lv.downloaded = info.downloadedBytes();
        lv.speed      = info.speedBps();

        boolean working = info.status() == CoreIPCClient.CORE_DOWNLOADING
                       || info.status() == CoreIPCClient.CORE_CONNECTING;
        boolean promote = false;
        long    now     = System.currentTimeMillis();
        boolean persist = now - lv.lastPersistMs >= PERSIST_EVERY_MS;

        if ((working && !lv.promoted) || persist) {
            synchronized (stateLock) {
                Download dl = repository.findById(id).orElse(null);
                if (dl != null) {
                    boolean dirty = false;
                    if (working && dl.getStatus() == STATUS_PENDING) {
                        dl.setStatus(STATUS_RUNNING);   /* the core is clearly at work even if its OK has not been seen */
                        dirty = promote = true;
                    }
                    if (persist && dl.getStatus() == STATUS_RUNNING) {
                        dl.setTotalSize(info.totalBytes());
                        dl.setDownloaded(info.downloadedBytes());
                        dirty = true;
                    }
                    if (dirty) repository.save(dl);
                    if (working) lv.promoted = true;
                    if (persist) lv.lastPersistMs = now;
                }
            }
            if (promote) broadcastState(id, STATUS_RUNNING, null);
        }

        Map<String, Object> msg = new HashMap<>();
        msg.put("type",            "progress");
        msg.put("id",              id);
        msg.put("status",          info.status());
        msg.put("totalBytes",      info.totalBytes());
        msg.put("downloadedBytes", info.downloadedBytes());
        msg.put("speedBps",        info.speedBps());
        msg.put("etaSeconds",      info.etaKnown() ? info.etaSeconds() : -1L);
        msg.put("activeSegments",  info.activeSegments());
        broadcast(id, msg);
    }

    @Override
    public void onComplete(CompleteInfo info) {
        long id = info.id();
        boolean changed = false;
        List<Admission> follow;
        synchronized (stateLock) {
            Download dl = repository.findById(id).orElse(null);
            if (dl != null && dl.getStatus() != STATUS_DONE) {
                dl.setStatus(STATUS_DONE);                /* whatever it was before: the file is complete */
                dl.setTotalSize(info.totalBytes());
                dl.setDownloaded(info.totalBytes());
                dl.setErrorMessage(null);
                repository.save(dl);
                changed = true;
            }
            live.remove(id);
            follow = queue.release(id);
        }
        if (changed) {
            Map<String, Object> msg = new HashMap<>();
            msg.put("type",       "complete");
            msg.put("id",         id);
            msg.put("totalBytes", info.totalBytes());
            msg.put("elapsedMs",  info.elapsedMs());
            msg.put("checksumOk", info.checksumOk());
            broadcast(id, msg);
            broadcastState(id, STATUS_DONE, null);
        }
        launchAll(follow);
    }

    @Override
    public void onError(long downloadId, int errorCode, String errorMessage) {
        failDownload(downloadId, errorMessage);
    }

    @Override
    public void onStatus(StatusInfo info) {
        Map<String, Object> msg = new HashMap<>();
        msg.put("type",            "status");
        msg.put("id",              info.id());
        msg.put("status",          info.status());
        msg.put("activeSegments",  info.activeSegments());
        msg.put("totalSegments",   info.totalSegments());
        msg.put("totalBytes",      info.totalBytes());
        msg.put("downloadedBytes", info.downloadedBytes());
        msg.put("speedBps",        info.speedBps());
        msg.put("etaSeconds",      info.etaKnown() ? info.etaSeconds() : -1L);
        broadcast(info.id(), msg);
    }

    @Override
    public void onConnected() {
        log.info("connected to fluxdm-core; reconciling downloads");
        ipc.requestStatusAll().whenComplete((res, ex) -> {
            if (ex == null && res.ok()) {
                try {
                    reconcile(res.statuses());
                } catch (RuntimeException e) {
                    log.error("reconciling downloads with fluxdm-core failed", e);
                    ipc.disconnect();                     /* the supervisor reconnects, which tries again */
                }
            } else if (ex == null && res.coreUnreachable()) {
                log.warn("lost fluxdm-core again before downloads could be reconciled");
            } else {
                log.error("fluxdm-core would not report its downloads ({}); reconnecting",
                          ex != null ? ex : res.errorMessage());
                ipc.disconnect();
            }
        });
    }

    @Override
    public void onConnectionLost(String reason) {
        synchronized (stateLock) {
            synced = false;
            live.clear();
        }
        broadcast(0, Map.of("type", "core", "state", "down", "reason", String.valueOf(reason)));
    }

    /* ═══ Reconciliation ════════════════════════════════════════════════════ */

    /** Brings the database, the slots and the core in step after a (re)connect.
     *  The core is the authority on what is running; the database on what is wanted. */
    private void reconcile(List<StatusInfo> rows) {
        List<Admission> toLaunch = new ArrayList<>();
        List<long[]>    changed  = new ArrayList<>();          /* {id, status} */

        synchronized (stateLock) {
            queue.reset();
            live.clear();

            Map<Long, StatusInfo> inCore = new HashMap<>();
            for (StatusInfo s : rows) inCore.put(s.id(), s);

            /* Failed downloads are included: if the core turns out to be running one (we gave up on it
             * after a timeout, and the core had received the command after all), the core is right. */
            List<Download> wanted = new ArrayList<>(repository.findByStatusIn(RECONCILED_STATES));
            wanted.sort(Comparator.<Download>comparingInt(d -> d.getPriority() != null ? d.getPriority() : 5).reversed()
                                  .thenComparingLong(Download::getId));

            /* Pass 1 — what the core knows: believe it. Running downloads take their slots first,
             * so that pass 2 cannot admit more than the limit allows. */
            List<Download> forgotten = new ArrayList<>();
            for (Download dl : wanted) {
                StatusInfo c = inCore.get(dl.getId());
                if (c == null || c.status() == CoreIPCClient.CORE_CANCELLED) { forgotten.add(dl); continue; }

                int before = dl.getStatus();
                switch (c.status()) {
                    case CoreIPCClient.CORE_IDLE, CoreIPCClient.CORE_CONNECTING, CoreIPCClient.CORE_DOWNLOADING -> {
                        queue.adopt(dl.getId());
                        dl.setStatus(STATUS_RUNNING);
                        dl.setErrorMessage(null);
                        dl.setTotalSize(c.totalBytes());
                        dl.setDownloaded(c.downloadedBytes());
                        Live lv = new Live();
                        lv.total = c.totalBytes(); lv.downloaded = c.downloadedBytes(); lv.speed = c.speedBps();
                        lv.promoted = true;
                        live.put(dl.getId(), lv);
                    }
                    case CoreIPCClient.CORE_PAUSED -> dl.setStatus(STATUS_PAUSED);
                    case CoreIPCClient.CORE_COMPLETE -> {
                        dl.setStatus(STATUS_DONE);
                        dl.setTotalSize(c.totalBytes());
                        dl.setDownloaded(c.totalBytes());
                        dl.setErrorMessage(null);
                    }
                    default -> {   /* CORE_ERROR */
                        dl.setStatus(STATUS_ERROR);
                        if (before != STATUS_ERROR)    /* an error we already know the reason for keeps it */
                            dl.setErrorMessage("failed while the daemon was not connected to fluxdm-core");
                    }
                }
                if (dl.getStatus() != before || c.status() == CoreIPCClient.CORE_IDLE
                    || c.status() == CoreIPCClient.CORE_CONNECTING || c.status() == CoreIPCClient.CORE_DOWNLOADING) {
                    repository.save(dl);
                    if (dl.getStatus() != before) changed.add(new long[] { dl.getId(), dl.getStatus() });
                }
            }

            /* Pass 2 — what the core has lost: start it again, or park it. */
            for (Download dl : forgotten) {
                if (dl.getStatus() == STATUS_PAUSED || dl.getStatus() == STATUS_ERROR)
                    continue;                                         /* nothing was running, nothing to restore */

                if (autoResume) {
                    if (dl.getStatus() != STATUS_PENDING) {
                        dl.setStatus(STATUS_PENDING);
                        dl.setErrorMessage(null);
                        repository.save(dl);
                        changed.add(new long[] { dl.getId(), STATUS_PENDING });
                    }
                    if (queue.submit(dl.getId(), dl.getPriority() != null ? dl.getPriority() : 5, Mode.START)
                            == Outcome.ADMITTED)
                        toLaunch.add(new Admission(dl.getId(), Mode.START));
                } else {
                    dl.setStatus(STATUS_PAUSED);
                    repository.save(dl);
                    changed.add(new long[] { dl.getId(), STATUS_PAUSED });
                }
            }

            for (StatusInfo s : rows) {
                if (!repository.existsById(s.id()) && s.status() != CoreIPCClient.CORE_COMPLETE
                    && s.status() != CoreIPCClient.CORE_ERROR)
                    log.warn("fluxdm-core is working on download {} which is not in the database; leaving it alone", s.id());
            }
            synced = true;
        }

        log.info("reconciled with fluxdm-core: {} running, {} to (re)start, {} waiting",
                 queue.activeCount() - toLaunch.size(), toLaunch.size(), queue.queuedCount());
        broadcast(0, Map.of("type", "core", "state", "up", "reason", "connected"));
        for (long[] c : changed) broadcastState(c[0], (int) c[1], null);
        launchAll(toLaunch);
    }

    /* ═══ Helpers ═══════════════════════════════════════════════════════════ */

    private Download decorate(Download d) {
        Live lv = d.getId() != null ? live.get(d.getId()) : null;
        d.setSpeedBps(lv != null ? lv.speed : 0L);
        return d;
    }

    private Download require(long id) {
        return repository.findById(id).map(this::decorate).orElseThrow(() -> DownloadException.notFound(id));
    }

    private void requireSynced() {
        if (!synced)
            throw DownloadException.unavailable(ipc.isConnected()
                ? "fluxdm-core is connected but the daemon is still synchronising with it; try again in a moment"
                : "fluxdm-core is not running or not connected; the daemon is trying to restart it");
    }

    private void awaitLaunch(long id) {
        CompletableFuture<Void> f = launching.get(id);
        if (f != null) awaitQuietly(f);
    }

    private static void awaitQuietly(CompletableFuture<Void> f) {
        try {
            f.get(WAIT_FOR_CORE_SECS, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (ExecutionException | TimeoutException e) {
            /* the outcome is applied (or logged) by the launch chain itself */
        }
    }

    private static AckResult await(CompletableFuture<AckResult> f) {
        try {
            return f.get(WAIT_FOR_CORE_SECS, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw DownloadException.unavailable("interrupted while waiting for fluxdm-core");
        } catch (ExecutionException | TimeoutException e) {
            throw DownloadException.unavailable("fluxdm-core did not answer");
        }
    }

    private static String describe(AckResult r) {
        String m = r.errorMessage();
        return m != null && !m.isBlank() ? m : "fluxdm-core error " + r.errorCode();
    }

    private static String truncate(String s) {
        return s.length() <= MAX_ERROR_CHARS ? s : s.substring(0, MAX_ERROR_CHARS - 1) + "…";
    }

    public static String statusName(int status) {
        return switch (status) {
            case STATUS_PENDING -> "pending";
            case STATUS_RUNNING -> "running";
            case STATUS_PAUSED  -> "paused";
            case STATUS_DONE    -> "complete";
            case STATUS_ERROR   -> "failed";
            default             -> "status " + status;
        };
    }

    /** Deletes the partial data of a cancelled download that the core no longer knew about. Only touches
     *  files that are provably a partial download: the data file is deleted only if the core's
     *  ".fluxpart" checkpoint sits next to it. */
    private void removeLeftovers(Download dl) {
        String path = dl.getFilePath();
        if (path == null) return;
        try {
            Path data    = Path.of(path);
            Path sidecar = Path.of(path + ".fluxpart");
            if (!Files.isRegularFile(sidecar)) return;
            Files.deleteIfExists(data);
            Files.deleteIfExists(sidecar);
            Files.deleteIfExists(Path.of(path + ".fluxpart.tmp"));
        } catch (IOException | InvalidPathException e) {
            log.warn("could not remove leftovers of cancelled download {}: {}", dl.getId(), e.getMessage());
        }
    }

    /** A new entity built only from fields that passed validation — nothing the client sent for
     *  id, status, progress or error is carried over. */
    private Download validated(Download req) {
        if (req == null) throw DownloadException.badRequest("a download is required");

        String url = req.getUrl() == null ? "" : req.getUrl().strip();
        if (url.isEmpty())                                   throw DownloadException.badRequest("url is required");
        if (url.getBytes(StandardCharsets.UTF_8).length > MAX_URL_BYTES)
                                                             throw DownloadException.badRequest("url is too long (at most " + MAX_URL_BYTES + " bytes)");
        if (hasControlChars(url) || !URL_SHAPE.matcher(url).matches())
                                                             throw DownloadException.badRequest("url must be an absolute http:// or https:// address without spaces");

        String raw = req.getFilePath() == null ? "" : req.getFilePath().strip();
        if (raw.isEmpty())                                   throw DownloadException.badRequest("filePath is required");
        if (raw.equals("~") || raw.startsWith("~/"))
            raw = System.getProperty("user.home") + raw.substring(1);
        if (hasControlChars(raw))                            throw DownloadException.badRequest("filePath contains control characters");
        if (!raw.startsWith("/"))                            throw DownloadException.badRequest("filePath must be an absolute path (start with / or ~/)");
        if (raw.endsWith("/"))                               throw DownloadException.badRequest("filePath must name a file, not a directory");
        String path;
        try {
            path = Path.of(raw).normalize().toString();
        } catch (InvalidPathException e) {
            throw DownloadException.badRequest("filePath is not a valid path");
        }
        if (path.length() < 2)                               throw DownloadException.badRequest("filePath must name a file");
        if (path.getBytes(StandardCharsets.UTF_8).length > MAX_PATH_BYTES)
                                                             throw DownloadException.badRequest("filePath is too long (at most " + MAX_PATH_BYTES + " bytes)");

        int segments = req.getSegments() == null || req.getSegments() <= 0 ? DEFAULT_SEGMENTS
                     : Math.min(req.getSegments(), MAX_SEGMENTS);
        int priority = req.getPriority() == null ? 5 : Math.max(1, Math.min(10, req.getPriority()));

        long limit = req.getBandwidthLimit();
        if (limit < 0)                                       throw DownloadException.badRequest("bandwidthLimit cannot be negative");

        int type = req.getChecksumType() == null ? 0 : req.getChecksumType();
        String checksum = null;
        if (type < 0 || type > 2)                            throw DownloadException.badRequest("checksumType must be 0 (none), 1 (md5) or 2 (sha256)");
        if (type != 0) {
            String c = req.getChecksum() == null ? "" : req.getChecksum().strip().toLowerCase();
            int want = type == 1 ? 32 : 64;
            if (c.length() != want || !HEX.matcher(c).matches())
                throw DownloadException.badRequest("checksum must be " + want + " hex digits for " + (type == 1 ? "md5" : "sha256"));
            checksum = c;
        }

        Download d = new Download();
        d.setUrl(url);
        d.setFilePath(path);
        d.setSegments(segments);
        d.setPriority(priority);
        d.setChecksumType(type);
        d.setChecksum(checksum);
        d.setBandwidthLimit(limit);
        d.setStatus(STATUS_PENDING);
        d.setTotalSize(0L);
        d.setDownloaded(0L);
        d.setErrorMessage(null);
        return d;
    }

    private static boolean hasControlChars(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 0x20 || c == 0x7f) return true;
        }
        return false;
    }

    /* ── Events for the UI ─────────────────────────────────────────────────── */

    private void broadcastState(long id, int status, String message) {
        Map<String, Object> msg = new HashMap<>();
        msg.put("type",   "state");
        msg.put("id",     id);
        msg.put("status", status);
        if (message != null) msg.put("message", message);
        broadcast(id, msg);
    }

    /** Per-download topic so the UI can subscribe to just what it is showing,
     *  plus a global topic for the list view. id 0 = about the daemon/core itself. */
    private void broadcast(long downloadId, Map<String, Object> payload) {
        try {
            if (downloadId != 0) messaging.convertAndSend("/topic/downloads/" + downloadId, payload);
            messaging.convertAndSend(downloadId != 0 ? "/topic/downloads" : "/topic/core", payload);
        } catch (RuntimeException e) {
            log.debug("could not push an event to the UI: {}", e.getMessage());
        }
    }
}
