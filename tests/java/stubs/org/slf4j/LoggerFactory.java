/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.slf4j;

public final class LoggerFactory {
    private LoggerFactory() {}

    private static final boolean VERBOSE = "debug".equals(System.getProperty("fluxdm.test.log"));

    public static Logger getLogger(Class<?> c) { return new Printer(c.getSimpleName()); }

    private static final class Printer implements Logger {
        private final String name;
        Printer(String name) { this.name = name; }

        private void out(String level, String format, Object[] args) {
            StringBuilder sb = new StringBuilder();
            int a = 0, from = 0;
            for (int i; (i = format.indexOf("{}", from)) >= 0 && a < args.length; ) {
                sb.append(format, from, i).append(args[a++]);
                from = i + 2;
            }
            sb.append(format.substring(from));
            System.err.println("[" + level + " " + name + "] " + sb);
            if (a < args.length && args[args.length - 1] instanceof Throwable t) t.printStackTrace();
        }
        public void debug(String f, Object... a) { if (VERBOSE) out("debug", f, a); }
        public void info (String f, Object... a) { if (VERBOSE) out("info",  f, a); }
        public void warn (String f, Object... a) { out("warn",  f, a); }
        public void error(String f, Object... a) { out("error", f, a); }
    }
}
