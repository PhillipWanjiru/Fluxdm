/* FluxDM — tests/java/harness/T.java
 * A tiny assertion helper in the same ✔/✘ style as the C and Python suites. */
package harness;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public final class T {
    private T() {}

    private static final AtomicInteger pass = new AtomicInteger();
    private static final AtomicInteger fail = new AtomicInteger();

    public static void section(String title) { System.out.println("\n── " + title); }

    public static boolean ok(boolean cond, String desc) {
        if (cond) { System.out.println("  ✔  " + desc); pass.incrementAndGet(); }
        else      { System.out.println("  ✘  " + desc); fail.incrementAndGet(); }
        return cond;
    }

    public static boolean eq(Object expected, Object actual, String desc) {
        boolean same = expected == null ? actual == null : expected.equals(actual);
        if (same) return ok(true, desc);
        System.out.println("  ✘  " + desc + "   (expected " + expected + ", got " + actual + ")");
        fail.incrementAndGet();
        return false;
    }

    /** Polls until cond holds or the timeout passes. */
    public static boolean waitFor(BooleanSupplier cond, long timeoutMs) {
        long end = System.nanoTime() + timeoutMs * 1_000_000L;
        while (System.nanoTime() < end) {
            try { if (cond.getAsBoolean()) return true; } catch (RuntimeException ignored) { /* not yet */ }
            try { Thread.sleep(5); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
        try { return cond.getAsBoolean(); } catch (RuntimeException e) { return false; }
    }

    public static boolean within(String desc, long timeoutMs, BooleanSupplier cond) {
        return ok(waitFor(cond, timeoutMs), desc);
    }

    public static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    public static int failures() { return fail.get(); }

    /** Prints the summary and returns the process exit code. */
    public static int finish(String suite) {
        System.out.println("\n─────────────────────────────");
        System.out.println("  " + suite);
        System.out.println("  Passed: " + pass.get());
        System.out.println("  Failed: " + fail.get());
        System.out.println("─────────────────────────────");
        System.out.println(fail.get() == 0 ? "ALL TESTS PASSED\n" : "FAILED\n");
        return fail.get() == 0 ? 0 : 1;
    }
}
