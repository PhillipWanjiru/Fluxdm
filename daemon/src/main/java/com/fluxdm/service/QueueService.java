/* FluxDM — service/QueueService.java
 * Bounded concurrency admission control for downloads. Previously this
 * queued incoming downloads but nothing ever drained the queue — every
 * download started immediately regardless of how many were already
 * running, making the class a no-op with respect to actual behavior.
 *
 * Now: only up to fluxdm.max-concurrent-downloads run at once. Anything
 * over that limit sits in the internal priority queue (highest priority
 * first, matching Download.priority) until a running download finishes
 * (success or error) and DownloadService calls releaseSlotAndAdmitNext()
 * to free a slot and pull the next item in.
 */
package com.fluxdm.service;

import com.fluxdm.model.Download;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.PriorityBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;

@Service
public class QueueService {

    @Value("${fluxdm.max-concurrent-downloads:4}")
    private int maxConcurrent;

    private final AtomicInteger activeCount = new AtomicInteger(0);

    private final PriorityBlockingQueue<Download> queue =
        new PriorityBlockingQueue<>(16,
            Comparator.comparingInt(d -> d.getPriority() != null ? d.getPriority() : 5));

    /**
     * Call when a download is first requested. Returns true if it was
     * admitted (caller should start it immediately via the IPC client),
     * or false if it was placed in the queue instead (caller must NOT
     * start it — it stays at whatever status the caller already set,
     * typically PENDING, until releaseSlotAndAdmitNext() admits it).
     */
    public synchronized boolean tryAdmit(Download dl) {
        if (activeCount.get() < maxConcurrent) {
            activeCount.incrementAndGet();
            return true;
        }
        queue.offer(dl);
        return false;
    }

    /**
     * Call when a running download finishes, successfully or not. Frees
     * its slot and, if anything is waiting, admits the next one by
     * priority. Returns the newly-admitted Download (caller must start
     * it via the IPC client), or null if the queue was empty.
     */
    public synchronized Download releaseSlotAndAdmitNext() {
        activeCount.decrementAndGet();
        Download next = queue.poll();
        if (next != null) activeCount.incrementAndGet();
        return next;
    }

    /** Remove a specific download from the wait queue (e.g. user cancelled
     *  it before it ever got a slot). Returns true if it was queued and
     *  is now removed; false if it wasn't in the queue (already running,
     *  or never existed). */
    public synchronized boolean removeFromQueue(Download dl) {
        return queue.remove(dl);
    }

    public int activeCount()        { return activeCount.get(); }
    public int maxConcurrent()      { return maxConcurrent; }
    public int queuedCount()        { return queue.size(); }
    public List<Download> snapshot(){ return new ArrayList<>(queue); }
}
