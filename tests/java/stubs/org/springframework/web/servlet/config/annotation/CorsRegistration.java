/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.servlet.config.annotation;

/** Records what was configured, so a test can look. */
public class CorsRegistration {
    public final String pathPattern;
    public String[]     origins = {};
    public String[]     methods = {};
    public String[]     headers = {};
    public long         maxAge  = -1;

    public CorsRegistration(String pathPattern) { this.pathPattern = pathPattern; }

    public CorsRegistration allowedOrigins(String... origins) { this.origins = origins; return this; }
    public CorsRegistration allowedMethods(String... methods) { this.methods = methods; return this; }
    public CorsRegistration allowedHeaders(String... headers) { this.headers = headers; return this; }
    public CorsRegistration maxAge(long maxAge)               { this.maxAge = maxAge;   return this; }
}
