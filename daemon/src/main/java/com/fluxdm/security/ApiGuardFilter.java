/* FluxDM — security/ApiGuardFilter.java
 * Puts ApiGuard in front of every HTTP request, before Spring MVC sees it.
 *
 * Two kinds of request are exempt from the token, because a browser cannot send one there:
 *   - a CORS preflight (OPTIONS with Origin and Access-Control-Request-Method). Spring answers it without running
 *     any application code. Its Host and Origin are still checked, so only the listed origins get an answer.
 *   - the WebSocket / SockJS endpoint (/ws...): browsers cannot add headers to a WebSocket handshake. The token
 *     is required in the STOMP CONNECT frame instead (StompAuthInterceptor), before anything can be subscribed to
 *     or sent.
 * The /ws exemption is decided on the request URI exactly as it was sent, and only if that is already in its plain
 * form: a URI like /ws/../api/downloads is not exempt, however a server might later resolve it.
 */
package com.fluxdm.security;

import com.fluxdm.security.ApiGuard.Verdict;
import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import org.springframework.core.Ordered;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;
import org.springframework.web.filter.OncePerRequestFilter;

import java.io.IOException;
import java.util.regex.Pattern;

@Component
@Order(Ordered.HIGHEST_PRECEDENCE + 10)
public class ApiGuardFilter extends OncePerRequestFilter {

    /** /ws and what lives under it, written plainly: segments of letters, digits, "_", "-" and inner dots. No "..",
     *  no ".", no empty segment, no escapes, no parameters. */
    private static final Pattern WEBSOCKET_PATH = Pattern.compile("/ws(?:/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*");

    private final ApiGuard guard;

    public ApiGuardFilter(ApiGuard guard) { this.guard = guard; }

    @Override
    protected void doFilterInternal(HttpServletRequest request, HttpServletResponse response, FilterChain chain)
            throws ServletException, IOException {
        String  origin    = request.getHeader("Origin");
        boolean preflight = "OPTIONS".equalsIgnoreCase(request.getMethod())
                         && origin != null
                         && request.getHeader("Access-Control-Request-Method") != null;
        boolean socket    = WEBSOCKET_PATH.matcher(request.getRequestURI()).matches();

        Verdict verdict = guard.check(request.getHeader("Host"), origin, request.getHeader("Authorization"),
                                      !preflight && !socket);
        if (verdict == Verdict.ALLOWED) {
            chain.doFilter(request, response);
            return;
        }
        guard.noteRejected(verdict, detail(verdict, request));
        reject(response, verdict, origin);
    }

    private static String detail(Verdict verdict, HttpServletRequest request) {
        return switch (verdict) {
            case BAD_HOST   -> "Host: "   + ApiGuard.printable(request.getHeader("Host"));
            case BAD_ORIGIN -> "Origin: " + ApiGuard.printable(request.getHeader("Origin"));
            default         -> request.getMethod() + " " + ApiGuard.printable(request.getRequestURI());
        };
    }

    private static void reject(HttpServletResponse response, Verdict verdict, String origin) throws IOException {
        response.setStatus(verdict.status());
        response.setContentType("application/json");
        response.setCharacterEncoding("UTF-8");
        response.setHeader("Cache-Control", "no-store");
        if (verdict == Verdict.UNAUTHORIZED) {
            response.setHeader("WWW-Authenticate", "Bearer realm=\"fluxdm\"");
            if (origin != null) {                       // it passed the origin check: let its page read the 401
                response.setHeader("Access-Control-Allow-Origin", origin);
                response.addHeader("Vary", "Origin");
            }
        }
        response.getWriter().write("{\"error\":\"" + verdict.message() + "\"}");
    }
}
