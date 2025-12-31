/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.boot;

public class SpringApplication {
    public static Object run(Class<?> primarySource, String... args) { return null; }
}
