/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package jakarta.servlet.http;

import jakarta.servlet.ServletResponse;

public interface HttpServletResponse extends ServletResponse {
    void setStatus(int status);
    void setHeader(String name, String value);
    void addHeader(String name, String value);
}
