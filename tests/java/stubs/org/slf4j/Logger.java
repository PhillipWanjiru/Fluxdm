/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.slf4j;

public interface Logger {
    void debug(String format, Object... args);
    void info(String format, Object... args);
    void warn(String format, Object... args);
    void error(String format, Object... args);
}
