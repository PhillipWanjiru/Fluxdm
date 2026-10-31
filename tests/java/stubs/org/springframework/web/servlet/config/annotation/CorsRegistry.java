/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.servlet.config.annotation;

import java.util.ArrayList;
import java.util.List;

public class CorsRegistry {
    /** What was added, so a test can look. */
    public final List<CorsRegistration> registrations = new ArrayList<>();

    public CorsRegistration addMapping(String pathPattern) {
        CorsRegistration r = new CorsRegistration(pathPattern);
        registrations.add(r);
        return r;
    }
}
