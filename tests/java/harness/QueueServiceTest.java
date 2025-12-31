/* FluxDM — tests/java/harness/QueueServiceTest.java
 * Slot bookkeeping: admission, ordering, idempotent release, adoption, concurrency. */
package harness;

import com.fluxdm.service.QueueService;
import com.fluxdm.service.QueueService.Admission;
import com.fluxdm.service.QueueService.Mode;
import com.fluxdm.service.QueueService.Outcome;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicInteger;

public class QueueServiceTest {

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM — QueueService tests");

        T.section("Admission up to the limit, then a waiting line");
        QueueService q = new QueueService(2);
        T.eq(Outcome.ADMITTED, q.submit(1, 5, Mode.START), "1st submit takes a slot");
        T.eq(Outcome.ADMITTED, q.submit(2, 5, Mode.START), "2nd submit takes a slot");
        T.eq(Outcome.QUEUED,   q.submit(3, 5, Mode.START), "3rd submit waits");
        T.eq(Outcome.QUEUED,   q.submit(4, 5, Mode.START), "4th submit waits");
        T.eq(2, q.activeCount(), "two active");
        T.eq(List.of(3L, 4L), q.waitingIds(), "waiting in arrival order");

        T.section("Releasing passes the slot on, exactly once");
        List<Admission> a = q.release(1);
        T.eq(List.of(new Admission(3, Mode.START)), a, "releasing 1 admits 3");
        T.eq(2, q.activeCount(), "still two active");
        T.eq(List.of(), q.release(1), "releasing 1 AGAIN admits nobody (idempotent)");
        T.eq(2, q.activeCount(), "active count did not drift");
        T.eq(List.of(4L), q.waitingIds(), "4 still waits");
        T.eq(List.of(), q.release(99), "releasing an unknown id changes nothing");
        T.eq(2, q.activeCount(), "active count still 2 after an unknown release");

        T.section("Leaving the line frees no slot");
        T.eq(List.of(), q.release(4), "cancelling a waiting download admits nobody");
        T.eq(0, q.queuedCount(), "the line is empty");
        T.eq(2, q.activeCount(), "both slots still in use");
        T.ok(q.isActive(2) && q.isActive(3) && !q.isActive(4), "isActive reflects the books");

        T.section("A free slot is taken at once; ids already known are not counted twice");
        q.release(2);
        T.eq(1, q.activeCount(), "one slot free after a release with an empty line");
        T.eq(Outcome.ALREADY_ACTIVE, q.submit(3, 5, Mode.START), "submitting a running id again is a no-op");
        T.eq(1, q.activeCount(), "...and does not take a second slot");
        T.eq(Outcome.ADMITTED, q.submit(5, 5, Mode.START), "a new id takes the free slot");
        T.eq(Outcome.QUEUED, q.submit(6, 5, Mode.RESUME), "next one waits");
        T.eq(Outcome.ALREADY_QUEUED, q.submit(6, 5, Mode.RESUME), "submitting a waiting id again keeps its place");
        T.eq(1, q.queuedCount(), "...and does not add a second entry");

        T.section("Order in line: higher priority first, FIFO among equals");
        QueueService p = new QueueService(1);
        p.submit(100, 5, Mode.START);
        p.submit(1, 5, Mode.START);
        p.submit(2, 9, Mode.START);
        p.submit(3, 5, Mode.START);
        p.submit(4, 9, Mode.START);
        p.submit(5, 1, Mode.START);
        T.eq(List.of(2L, 4L, 1L, 3L, 5L), p.waitingIds(), "priority 9 before 5 before 1; arrival order within each");
        T.eq(List.of(new Admission(2, Mode.START)), p.release(100), "first admitted is the highest priority");
        T.eq(List.of(new Admission(4, Mode.START)), p.release(2), "then the next 9");
        p.submit(1, 10, Mode.START);
        T.eq(List.of(1L, 3L, 5L), p.waitingIds(), "re-submitting with a new priority re-sorts, keeping arrival order");
        T.eq(List.of(new Admission(1, Mode.START)), p.release(4), "the re-prioritised one goes first");

        T.section("RESUME vs START for a waiting download");
        QueueService m = new QueueService(1);
        m.submit(1, 5, Mode.START);
        m.submit(2, 5, Mode.RESUME);
        T.eq(List.of(new Admission(2, Mode.RESUME)), m.release(1), "mode is remembered through the wait");
        m.submit(3, 5, Mode.RESUME);
        m.submit(3, 5, Mode.START);
        T.eq(List.of(new Admission(3, Mode.START)), m.release(2), "START overrides a waiting RESUME");

        T.section("adopt(): downloads already running in the core");
        QueueService ad = new QueueService(2);
        ad.adopt(1); ad.adopt(2); ad.adopt(3);
        T.eq(3, ad.activeCount(), "adopted downloads take slots even beyond the limit");
        T.eq(Outcome.QUEUED, ad.submit(4, 5, Mode.START), "a newcomer waits");
        T.eq(List.of(), ad.release(1), "releasing one over-limit slot does not admit the newcomer");
        T.eq(List.of(new Admission(4, Mode.START)), ad.release(2), "...releasing the next does");
        ad.submit(7, 5, Mode.START); ad.adopt(7);
        T.ok(ad.isActive(7) && !ad.isWaiting(7), "adopting a waiting id moves it to active");

        T.section("Limit changes and reset");
        QueueService lim = new QueueService(1);
        lim.submit(1, 5, Mode.START); lim.submit(2, 5, Mode.START); lim.submit(3, 5, Mode.START);
        T.eq(List.of(new Admission(2, Mode.START), new Admission(3, Mode.START)), lim.setMaxConcurrent(3), "raising the limit admits waiters");
        T.eq(List.of(), lim.setMaxConcurrent(1), "lowering it admits nobody and evicts nobody");
        T.eq(3, lim.activeCount(), "running downloads are not touched");
        lim.reset();
        T.ok(lim.activeCount() == 0 && lim.queuedCount() == 0, "reset empties everything");
        T.eq(1, new QueueService(0).maxConcurrent(), "a limit below 1 is raised to 1");

        T.section("Concurrent use: every id admitted exactly once, nothing leaks");
        QueueService c = new QueueService(3);
        Set<Long> everAdmitted = ConcurrentHashMap.newKeySet();
        AtomicInteger doubleAdmit = new AtomicInteger();
        AtomicInteger overLimit = new AtomicInteger();
        final int N = 400, THREADS = 8;
        CountDownLatch go = new CountDownLatch(1), done = new CountDownLatch(THREADS);
        List<Long> ids = new ArrayList<>();
        for (long i = 1; i <= N; i++) ids.add(i);

        for (int t = 0; t < THREADS; t++) {
            final int tt = t;
            Thread th = new Thread(() -> {
                try {
                    go.await();
                    for (int i = tt; i < N; i += THREADS) {
                        long id = ids.get(i);
                        if (c.submit(id, ThreadLocalRandom.current().nextInt(1, 10), Mode.START) == Outcome.ADMITTED) {
                            if (!everAdmitted.add(id)) doubleAdmit.incrementAndGet();
                        }
                        if (c.activeCount() > 3) overLimit.incrementAndGet();
                    }
                } catch (InterruptedException ignored) { /* test ends */ }
                finally { done.countDown(); }
            });
            th.start();
        }
        go.countDown();
        done.await();

        /* Drain: release whatever is active until nothing is left, collecting admissions. */
        int guard = 0;
        while ((c.activeCount() > 0 || c.queuedCount() > 0) && guard++ < 10 * N) {
            for (long id : c.activeIds()) {
                for (Admission adm : c.release(id))
                    if (!everAdmitted.add(adm.id())) doubleAdmit.incrementAndGet();
            }
        }
        T.eq(0, doubleAdmit.get(), "no download was admitted twice");
        T.eq(0, overLimit.get(), "never more than the limit active (bounded submits)");
        T.eq(N, everAdmitted.size(), "all " + N + " downloads were admitted exactly once");
        T.ok(c.activeCount() == 0 && c.queuedCount() == 0, "queue ends empty");

        System.exit(T.finish("QueueService"));
    }
}
