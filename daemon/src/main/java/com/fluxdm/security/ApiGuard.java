/* FluxDM — security/ApiGuard.java
 * Decides whether a request to the local API may proceed.
 *
 * The API starts downloads and writes files where it is told to, and it listens on a loopback port that every
 * program and every web page open on this machine can reach. Three independent checks stand between them and it:
 *
 *   1. Host    must name this machine (127.0.0.1, localhost, [::1]). A web page can make its own host name resolve
 *              to 127.0.0.1 ("DNS rebinding") and so talk to the API as if it were the page's own origin; such a
 *              request still carries the page's host name, which is not on the list.
 *   2. Origin  if the request has one, it must be on the list. Browsers add Origin to every cross-site request,
 *              "simple" ones that need no CORS preflight included, so no web page can post to the API.
 *   3. Token   "Authorization: Bearer <token>", the token being the file only this user can read (ApiToken).
 *              This is what stops other programs on the machine, and it holds even if 1 and 2 are misconfigured.
 *
 * The checks are plain functions of request headers, so the HTTP filter (ApiGuardFilter), the WebSocket
 * interceptor (StompAuthInterceptor) and the tests all use this one class.
 *
 * Settings (application.properties):
 *   fluxdm.api.allowed-hosts    host names, no port: the port does not matter, the name does
 *   fluxdm.api.allowed-origins  exact origins, e.g. http://localhost:3000; wildcards are refused
 *   fluxdm.api.auth.enabled     false turns the token check off (development only); 1 and 2 stay on
 *   fluxdm.api.token-file       where the token lives; blank = ~/.fluxdm/api-token
 */
package com.fluxdm.security;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.regex.Pattern;

@Component
public class ApiGuard {
    private static final Logger log = LoggerFactory.getLogger(ApiGuard.class);

    public static final String DEFAULT_HOSTS   = "127.0.0.1,localhost,[::1]";
    public static final String DEFAULT_ORIGINS = "http://localhost:3000,app://-";

    /** The decision, with the HTTP status that goes with it. */
    public enum Verdict {
        ALLOWED(200, "allowed"),
        BAD_HOST(403, "host not allowed"),
        BAD_ORIGIN(403, "origin not allowed"),
        UNAUTHORIZED(401, "missing or invalid API token");

        private final int    status;
        private final String message;

        Verdict(int status, String message) { this.status = status; this.message = message; }

        public int    status()  { return status; }
        /** A fixed text that is safe to send back: it never repeats anything the client wrote. */
        public String message() { return message; }
    }

    private static final Pattern PORT         = Pattern.compile(":[0-9]{1,5}");
    private static final Pattern HOST_ENTRY   = Pattern.compile("[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?|\\[[0-9a-f:.]+\\]");
    private static final Pattern ORIGIN_ENTRY = Pattern.compile(
            "[a-z][a-z0-9+.-]*://(?:[a-z0-9._~-]+|\\[[0-9a-f:.]+\\])(?::[0-9]{1,5})?");

    private static final long LOG_EVERY_MS = 30_000L;

    private final boolean        authEnabled;
    private final ApiToken       token;            // null when authentication is off
    private final Set<String>    hosts;
    private final List<String>   origins;
    private final AtomicLongArray lastLogged = new AtomicLongArray(Verdict.values().length);

    public ApiGuard(@Value("${fluxdm.api.auth.enabled:true}") boolean authEnabled,
                    @Value("${fluxdm.api.token-file:}") String tokenFile,
                    @Value("${fluxdm.api.allowed-hosts:" + DEFAULT_HOSTS + "}") String allowedHosts,
                    @Value("${fluxdm.api.allowed-origins:" + DEFAULT_ORIGINS + "}") String allowedOrigins) {
        List<String> h = parseList("fluxdm.api.allowed-hosts", allowedHosts, HOST_ENTRY,
                                   "a host name or address (no port, no wildcard), e.g. localhost");
        if (h.isEmpty()) {
            throw new IllegalStateException("fluxdm.api.allowed-hosts is empty, so every request would be refused; "
                                            + "the default is " + DEFAULT_HOSTS);
        }
        this.hosts       = Set.copyOf(h);
        this.origins     = List.copyOf(parseList("fluxdm.api.allowed-origins", allowedOrigins, ORIGIN_ENTRY,
                                   "an origin such as http://localhost:3000 (scheme, host, optional port; "
                                   + "no path, no wildcard)"));
        this.authEnabled = authEnabled;
        this.token       = authEnabled ? openToken(tokenFile) : null;
        if (!authEnabled) {
            log.warn("API authentication is OFF (fluxdm.api.auth.enabled=false): any program on this machine can "
                   + "control your downloads. The host and origin checks still apply. Use this for development only.");
        }
    }

    /* ── The decision ──────────────────────────────────────────────────────────────────────────────────────── */

    /**
     * @param host          the Host header, or null
     * @param origin        the Origin header, or null
     * @param authorization the Authorization header, or null
     * @param needsToken    false where the browser cannot send one: CORS preflights, and the WebSocket handshake
     *                      (the token is then required in the STOMP CONNECT frame, see StompAuthInterceptor)
     */
    public Verdict check(String host, String origin, String authorization, boolean needsToken) {
        String name = hostName(host);
        if (name == null || !hosts.contains(name))                 return Verdict.BAD_HOST;
        if (origin != null && !origins.contains(origin))           return Verdict.BAD_ORIGIN;
        if (needsToken && !authorized(authorization))              return Verdict.UNAUTHORIZED;
        return Verdict.ALLOWED;
    }

    /** True if the Authorization header carries the token, or if authentication is switched off. */
    public boolean authorized(String authorization) {
        return !authEnabled || token.matches(bearer(authorization));
    }

    public boolean authEnabled() { return authEnabled; }

    /** The origins the API serves to browsers; the CORS and WebSocket settings take their list from here. */
    public String[] allowedOrigins() { return origins.toArray(new String[0]); }

    /** Where the token is kept, or null if authentication is off. */
    public Path tokenFile() { return token == null ? null : token.file(); }

    /* ── Header parsing ────────────────────────────────────────────────────────────────────────────────────── */

    /** The host name in a Host header value, lower-case and without the port; null if the value is not a plausible
     *  one. Anything but "name" or "name:digits" is refused, so "evil.example:80@127.0.0.1" never reduces to a
     *  name on the list. */
    static String hostName(String header) {
        if (header == null) return null;
        String h = header.strip().toLowerCase(Locale.ROOT);
        if (h.isEmpty()) return null;
        int end;
        if (h.charAt(0) == '[') {                                  // an IPv6 literal: [::1] or [::1]:7171
            int close = h.indexOf(']');
            if (close < 0) return null;
            end = close + 1;
        } else {
            int colon = h.lastIndexOf(':');
            end = colon < 0 ? h.length() : colon;
        }
        return end < h.length() && !PORT.matcher(h.substring(end)).matches() ? null : h.substring(0, end);
    }

    /** The credential in an "Authorization: Bearer <credential>" value (the scheme name is case-insensitive),
     *  or null if it is some other scheme or has no credential. */
    static String bearer(String authorization) {
        if (authorization == null || !authorization.regionMatches(true, 0, "Bearer ", 0, 7)) return null;
        String credential = authorization.substring(7).strip();
        return credential.isEmpty() ? null : credential;
    }

    /* ── Reporting ─────────────────────────────────────────────────────────────────────────────────────────── */

    /** Log a refusal, but at most once every 30 s per kind, so a page that keeps trying cannot flood the log. */
    public void noteRejected(Verdict verdict, String detail) {
        int  i    = verdict.ordinal();
        long now  = System.currentTimeMillis();
        long last = lastLogged.get(i);
        if (now - last >= LOG_EVERY_MS && lastLogged.compareAndSet(i, last, now)) {
            log.warn("refused a request: {} ({}). Further refusals of this kind are logged at most every 30 s.",
                     verdict.message(), detail);
        }
    }

    /** Text from a request, made safe for a log line: printable ASCII only, and not too long. */
    public static String printable(String s) {
        if (s == null) return "none";
        StringBuilder b = new StringBuilder(Math.min(s.length(), 100));
        for (int i = 0; i < s.length() && i < 100; i++) {
            char c = s.charAt(i);
            b.append(c >= 0x20 && c < 0x7f ? c : '?');
        }
        return s.length() > 100 ? b.append("...").toString() : b.toString();
    }

    /* ── Setup ─────────────────────────────────────────────────────────────────────────────────────────────── */

    private static List<String> parseList(String property, String text, Pattern shape, String expected) {
        List<String> out = new ArrayList<>();
        for (String raw : text.split(",")) {
            String entry = raw.strip().toLowerCase(Locale.ROOT);
            if (entry.isEmpty()) continue;
            if (!shape.matcher(entry).matches()) {
                throw new IllegalStateException(property + ": \"" + raw.strip() + "\" is not " + expected);
            }
            if (!out.contains(entry)) out.add(entry);
        }
        return out;
    }

    private static ApiToken openToken(String tokenFile) {
        Path file = tokenFile == null || tokenFile.isBlank()
                  ? Path.of(System.getProperty("user.home"), ".fluxdm", "api-token")
                  : Path.of(tokenFile.strip());
        try {
            ApiToken t = ApiToken.loadOrCreate(file);
            log.info("API authentication is on: clients send \"Authorization: Bearer <token>\"; the token is in {}",
                     t.file());
            return t;
        } catch (IOException e) {
            throw new IllegalStateException("cannot prepare the API token file " + file + ": " + e.getMessage()
                    + " (fix that or set fluxdm.api.token-file; for development only, fluxdm.api.auth.enabled=false)", e);
        }
    }
}
