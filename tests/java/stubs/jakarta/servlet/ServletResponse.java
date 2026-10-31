/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package jakarta.servlet;

import java.io.IOException;
import java.io.PrintWriter;

public interface ServletResponse {
    void setContentType(String type);
    void setCharacterEncoding(String encoding);
    PrintWriter getWriter() throws IOException;
}
