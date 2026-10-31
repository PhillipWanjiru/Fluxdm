/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.filter;

import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.ServletRequest;
import jakarta.servlet.ServletResponse;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.io.IOException;

/** Like the real one, doFilter() hands HTTP requests to doFilterInternal(); the "once per request"
 *  bookkeeping is left out. */
public abstract class OncePerRequestFilter {
    public final void doFilter(ServletRequest request, ServletResponse response, FilterChain chain)
            throws ServletException, IOException {
        if (!(request instanceof HttpServletRequest http) || !(response instanceof HttpServletResponse res)) {
            throw new ServletException("OncePerRequestFilter only supports HTTP requests");
        }
        doFilterInternal(http, res, chain);
    }

    protected abstract void doFilterInternal(HttpServletRequest request, HttpServletResponse response,
                                             FilterChain chain) throws ServletException, IOException;
}
