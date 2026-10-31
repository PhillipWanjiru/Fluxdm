/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package jakarta.servlet.http;

import jakarta.servlet.ServletRequest;

public interface HttpServletRequest extends ServletRequest {
    String getHeader(String name);
    String getMethod();
    String getRequestURI();
}
