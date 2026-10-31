/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package jakarta.servlet;

public class ServletException extends Exception {
    private static final long serialVersionUID = 1L;
    public ServletException() { super(); }
    public ServletException(String message) { super(message); }
    public ServletException(String message, Throwable cause) { super(message, cause); }
}
