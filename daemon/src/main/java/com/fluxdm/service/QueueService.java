/* FluxDM — service/QueueService.java
 * Bounded concurrency for downloads: at most fluxdm.max-concurrent-downloads
 * hold a slot at once, everything else waits in line.
 *
 * Slots are tracked BY DOWNLOAD ID, not by counting. Every operation is
 * idempotent: releasing a download that holds no slot (a second "finished"
 * signal, a cancel racing a failure, a pause of something that was only
 * waiting) changes nothing, so the active count can never drift below the
 * truth and admit more downloads than the limit allows.
 *
 *   submit(id, ...)   asks for a slot: ADMITTED (caller launches it) or QUEUED
 *   release(id)       gives the slot back (or leaves the line) and returns the
 *                     downloads that can start because of it — the caller
 *                     launches those
 *   adopt(id)         records a download that is already running in the core
 *
 * Order in line: higher priority first (Download.priority, 1..10), first come
 * first served among equals.
 *
 * Invariant: the line is non-empty only while every slot is taken, so a free
 * slot always means an empty line.
 *
 * The class only does bookkeeping. It never starts anything itself, so it holds
 * no locks while the caller talks to the core.
 */
package com.fluxdm.service;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

@Service
public class QueueService {

    /** How a download that is given a slot has to be launched. */
    public enum Mode {
        /** Never sent to the core, or the core no longer knows it. */
        START,
        /** Paused or failed in the core; ask it to carry on (the caller falls back to START if it has forgotten it). */
        RESUME
    }

    public enum Outcome { ADMITTED, QUEUED, ALREADY_ACTIVE, ALREADY_QUEUED }

    /** A download that has just been given a slot and must now be launched. */
    public record Admission(long id, Mode mode) {}

    private static final class Waiting {
        final long id;
        final long seq;
        int        priority;
        Mode       mode;

        Waiting(long id, int priority, long seq, Mode mode) {
            this.id = id; this.priority = priority; this.seq = seq; this.mode = mode;
        }
    }

    private final Object lock = new Object();
    private int maxConcurrent;
    private long nextSeq;

    private final Set<Long> active = new HashSet<>();
    private final Map<Long, Waiting> waitingById = new HashMap<>();
    private final TreeSet<Waiting> line = new TreeSet<>((a, b) -> {
        if (a.priority != b.priority) return Integer.compare(b.priority, a.priority);   /* higher first */
        return Long.compare(a.seq, b.seq);                                              /* then arrival order */
    });

    public QueueService(@Value("${fluxdm.max-concurrent-downloads:4}") int maxConcurrent) {
        this.maxConcurrent = Math.max(1, maxConcurrent);
    }

    /** Asks for a slot. Safe to repeat: a download that already has a slot or a place in line keeps it. */
    public Outcome submit(long id, int priority, Mode mode) {
        synchronized (lock) {
            if (active.contains(id)) return Outcome.ALREADY_ACTIVE;

            Waiting w = waitingById.get(id);
            if (w != null) {
                if (mode == Mode.START) w.mode = Mode.START;       /* START works from every state, RESUME only from some */
                if (priority != w.priority) {                      /* new priority, same arrival order */
                    line.remove(w);
                    w.priority = priority;
                    line.add(w);
                }
                return Outcome.ALREADY_QUEUED;
            }

            if (active.size() < maxConcurrent) {
                active.add(id);
                return Outcome.ADMITTED;
            }
            Waiting nw = new Waiting(id, priority, nextSeq++, mode);
            line.add(nw);
            waitingById.put(id, nw);
            return Outcome.QUEUED;
        }
    }

    /** Gives the download's slot back, or removes it from the line if it never had one.
     *  Returns what can start now. Releasing an unknown id returns an empty list. */
    public List<Admission> release(long id) {
        synchronized (lock) {
            Waiting w = waitingById.remove(id);
            if (w != null) {
                line.remove(w);
                return List.of();                                  /* it never held a slot, so none is freed */
            }
            if (!active.remove(id)) return List.of();
            return promote();
        }
    }

    /** Records a download that is already running in the core. It takes a slot even
     *  when that exceeds the limit: the work is happening, the books must say so. */
    public void adopt(long id) {
        synchronized (lock) {
            Waiting w = waitingById.remove(id);
            if (w != null) line.remove(w);
            active.add(id);
        }
    }

    /** Forgets everything. Used when the core's state has to be rebuilt from scratch. */
    public void reset() {
        synchronized (lock) {
            active.clear();
            waitingById.clear();
            line.clear();
        }
    }

    /** Changes the limit. Returns what can start now if it grew. */
    public List<Admission> setMaxConcurrent(int max) {
        synchronized (lock) {
            this.maxConcurrent = Math.max(1, max);
            return promote();
        }
    }

    public boolean isActive(long id)  { synchronized (lock) { return active.contains(id); } }
    public boolean isWaiting(long id) { synchronized (lock) { return waitingById.containsKey(id); } }
    public int  activeCount()         { synchronized (lock) { return active.size(); } }
    public int  queuedCount()         { synchronized (lock) { return line.size(); } }
    public int  maxConcurrent()       { synchronized (lock) { return maxConcurrent; } }

    /** Ids waiting for a slot, in the order they will be admitted. */
    public List<Long> waitingIds() {
        synchronized (lock) {
            List<Long> out = new ArrayList<>(line.size());
            for (Waiting w : line) out.add(w.id);
            return out;
        }
    }

    /** Ids holding a slot. */
    public Set<Long> activeIds() {
        synchronized (lock) { return new HashSet<>(active); }
    }

    private List<Admission> promote() {
        List<Admission> out = null;
        while (active.size() < maxConcurrent && !line.isEmpty()) {
            Waiting w = line.pollFirst();
            waitingById.remove(w.id);
            active.add(w.id);
            if (out == null) out = new ArrayList<>(2);
            out.add(new Admission(w.id, w.mode));
        }
        return out == null ? List.of() : out;
    }
}
