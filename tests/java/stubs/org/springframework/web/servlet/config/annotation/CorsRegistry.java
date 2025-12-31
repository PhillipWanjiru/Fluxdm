/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.servlet.config.annotation;

public class CorsRegistry {
    public CorsRegistration addMapping(String pathPattern) { return new CorsRegistration(); }
}
