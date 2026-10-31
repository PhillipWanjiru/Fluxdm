/* FluxDM — tests/java/harness/ApiSecurityTest.java
 * The defences of the local API: the private runtime directory, the token file, the Host / Origin / token
 * decisions, the HTTP filter, the STOMP interceptor, and the settings that tie them together.
 *
 * Everything here runs the real classes of daemon/src/main. What it cannot show is Spring itself calling them
 * (filter registration, interceptor order, header handling inside Tomcat): the request filter is driven through
 * stand-in servlet objects and the STOMP interceptor through stand-in messages. tests/smoke_api.sh checks the
 * running daemon the way a browser or curl would. */
package harness;

import com.fluxdm.config.CorsConfig;
import com.fluxdm.config.WebSocketConfig;
import com.fluxdm.security.ApiGuard;
import com.fluxdm.security.ApiGuard.Verdict;
import com.fluxdm.security.ApiGuardFilter;
import com.fluxdm.security.ApiToken;
import com.fluxdm.security.RuntimeDir;
import com.fluxdm.security.StompAuthInterceptor;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.messaging.Message;
import org.springframework.messaging.MessageHeaders;
import org.springframework.messaging.MessagingException;
import org.springframework.messaging.simp.config.ChannelRegistration;
import org.springframework.messaging.simp.stomp.StompCommand;
import org.springframework.messaging.simp.stomp.StompHeaderAccessor;
import org.springframework.messaging.support.MessageHeaderAccessor;
import org.springframework.web.servlet.config.annotation.CorsRegistry;
import org.springframework.web.socket.config.annotation.StompWebSocketEndpointRegistration;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.Method;
import java.lang.reflect.Parameter;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.security.Principal;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;

public class ApiSecurityTest {

    static Path root;
    static int  seq;

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM — local API security tests");
        root = scratchRoot();
        try {
            quietly(ApiSecurityTest::runtimeDir);
            quietly(ApiSecurityTest::apiToken);
            quietly(ApiSecurityTest::decisions);
            quietly(ApiSecurityTest::guardSetup);
            quietly(ApiSecurityTest::httpFilter);
            quietly(ApiSecurityTest::stompInterceptor);
            quietly(ApiSecurityTest::wiring);
        } catch (Throwable t) {
            T.ok(false, "the suite aborted with " + t);
            t.printStackTrace(System.out);
        } finally {
            deleteTree(root);
        }
        System.exit(T.finish("ApiSecurity"));
    }

    /* ═══ Helpers ═══ */

    /** A scratch directory with a short path: the socket paths made inside it are limited to 107 bytes. */
    static Path scratchRoot() throws IOException {
        Path base = Path.of(System.getProperty("java.io.tmpdir"));
        if (base.toString().length() > 40) base = Path.of("/tmp");
        return Files.createTempDirectory(base, "fxs");
    }

    interface Thrower { void run() throws Exception; }

    static Throwable thrown(Thrower t) {
        try { t.run(); return null; } catch (Throwable e) { return e; }
    }

    static boolean says(Throwable t, String text) { return t != null && String.valueOf(t.getMessage()).contains(text); }

    static Path fresh(String name) throws IOException { return Files.createDirectory(root.resolve(name + "-" + (++seq))); }

    static String perms(Path p) throws IOException {
        return PosixFilePermissions.toString(Files.getPosixFilePermissions(p, LinkOption.NOFOLLOW_LINKS));
    }

    static void chmod(Path p, String mode) throws IOException {
        Files.setPosixFilePermissions(p, PosixFilePermissions.fromString(mode));
    }

    static Map<String, String> env(String xdg, String home) {
        Map<String, String> e = new HashMap<>();
        if (xdg != null)  e.put("XDG_RUNTIME_DIR", xdg);
        if (home != null) e.put("HOME", home);
        return e;
    }

    static boolean isRoot() { return "root".equals(System.getProperty("user.name")); }

    static UserPrincipal lookup(String user) {
        try {
            return FileSystems.getDefault().getUserPrincipalLookupService().lookupPrincipalByName(user);
        } catch (IOException e) {
            return null;
        }
    }

    /** Runs {@code body} and returns what it printed on stderr (where the log lines go). */
    static String stderrOf(Thrower body) throws Exception {
        PrintStream old = System.err;
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        System.setErr(new PrintStream(buf, true, StandardCharsets.UTF_8));
        try {
            body.run();
        } finally {
            System.setErr(old);
        }
        return buf.toString(StandardCharsets.UTF_8);
    }

    /** Runs a section with the log lines it provokes (refusals, warnings) held back, and checks that nothing else,
     *  an error or a stack trace above all, was logged. */
    static void quietly(Thrower section) throws Exception {
        String log = stderrOf(section);
        boolean unexpected = log.lines().anyMatch(l -> l.startsWith("[error") || l.startsWith("\tat ") || l.contains("Exception"));
        T.ok(!unexpected, "nothing unexpected was logged");
        if (unexpected) System.out.println(log);
    }

    /** What RuntimeDir.ensure() chose, and what it logged while choosing. */
    record Chosen(Path dir, String log) {}

    static Chosen choose(Map<String, String> env, String home) throws Exception {
        Path[] dir = new Path[1];
        String log = stderrOf(() -> dir[0] = RuntimeDir.ensure(env, home));
        return new Chosen(dir[0], log);
    }

    static long count(String text, String needle) {
        long n = 0;
        for (int i = text.indexOf(needle); i >= 0; i = text.indexOf(needle, i + needle.length())) n++;
        return n;
    }

    static void deleteTree(Path dir) {
        try (Stream<Path> s = Files.walk(dir)) {
            s.sorted((a, b) -> b.getNameCount() - a.getNameCount()).forEach(p -> {
                try { Files.deleteIfExists(p); } catch (IOException ignored) { /* best effort */ }
            });
        } catch (IOException ignored) { /* already gone */ }
    }

    static void note(String text) { System.out.println("  ·  " + text); }

    /* ═══ RuntimeDir ═══ */

    static void runtimeDir() throws Exception {
        T.section("Runtime directory: where the socket may live");

        Path home = fresh("home"), xdg = fresh("xdg");
        chmod(xdg, "rwx------");
        Map<String, String> e = env(xdg.toString(), home.toString());
        Path got = RuntimeDir.ensure(e, home.toString());
        T.eq(xdg.resolve("fluxdm"), got, "a private $XDG_RUNTIME_DIR is used: <xdg>/fluxdm");
        T.eq("rwx------", perms(got), "...and the directory is created closed to everyone else");
        T.ok(!Files.exists(home.resolve(".fluxdm")), "...without touching the home directory");
        T.eq(got, RuntimeDir.ensure(e, home.toString()), "asking again gives the same directory");
        chmod(got, "rwxrwxrwx");
        RuntimeDir.ensure(e, home.toString());
        T.eq("rwx------", perms(got), "a directory of ours that was left open is closed again");

        /* A runtime directory that is not private is not trusted: others could rename or replace what is in it. */
        for (String mode : new String[] {"rwxr-xr-x", "rwxr-x---", "rwx--x--x", "rwx-----x"}) {
            Path h = fresh("home"), x = fresh("xdg");
            chmod(x, mode);
            Chosen c = choose(env(x.toString(), h.toString()), h.toString());
            T.eq(h.resolve(".fluxdm/run"), c.dir(), "$XDG_RUNTIME_DIR with mode " + mode + " is not trusted: ~/.fluxdm/run is used");
            T.eq("rwx------", perms(c.dir()), "...and created private");
            T.ok(!Files.exists(x.resolve("fluxdm")), "...and nothing was put in the open directory");
            T.ok(c.log.contains("XDG_RUNTIME_DIR (" + x + ")"), "...and the log says why");
        }

        Path h1 = fresh("home");
        T.eq(h1.resolve(".fluxdm/run"), RuntimeDir.ensure(env(null, h1.toString()), h1.toString()),
             "without $XDG_RUNTIME_DIR the socket goes to ~/.fluxdm/run");
        Path h2 = fresh("home");
        T.eq(h2.resolve(".fluxdm/run"), choose(env("run/user/1000", h2.toString()), h2.toString()).dir(),
             "a relative $XDG_RUNTIME_DIR is ignored");
        Path h3 = fresh("home");
        T.eq(h3.resolve(".fluxdm/run"), choose(env(root.resolve("no-such-dir").toString(), h3.toString()), h3.toString()).dir(),
             "a $XDG_RUNTIME_DIR that does not exist is ignored");
        Path h4 = fresh("home");
        Path file = Files.createFile(root.resolve("a-file-" + (++seq)));
        T.eq(h4.resolve(".fluxdm/run"), choose(env(file.toString(), h4.toString()), h4.toString()).dir(),
             "a $XDG_RUNTIME_DIR that is a file is ignored");
        Path h5 = fresh("home");
        Chosen empty = choose(env("", h5.toString()), h5.toString());
        T.eq(h5.resolve(".fluxdm/run"), empty.dir(), "an empty $XDG_RUNTIME_DIR is ignored");
        T.eq("", empty.log, "...without a complaint, since empty means unset");

        /* The home directory: $HOME first, then user.home, and it has to be an absolute path. */
        Path h6 = fresh("home"), other = fresh("home");
        T.eq(h6.resolve(".fluxdm/run"), RuntimeDir.ensure(env(null, h6.toString()), other.toString()), "$HOME wins over user.home");
        T.eq(other.resolve(".fluxdm/run"), RuntimeDir.ensure(env(null, null), other.toString()), "user.home is used when $HOME is unset");
        T.eq(other.resolve(".fluxdm/run"), RuntimeDir.ensure(env(null, "relative/home"), other.toString()), "...or when $HOME is not absolute");
        Throwable noHome = thrown(() -> RuntimeDir.ensure(env(null, null), null));
        T.ok(noHome instanceof IOException && says(noHome, "home directory"), "no usable home directory: an error that says so (" + noHome + ")");
        T.ok(thrown(() -> RuntimeDir.ensure(env(null, "rel"), "also/relative")) instanceof IOException, "a relative user.home is refused as well");

        /* What is already at ~/.fluxdm/run. */
        Path h7 = fresh("home");
        Path run7 = Files.createDirectories(h7.resolve(".fluxdm/run"));
        chmod(run7, "rwxrwxrwx");
        RuntimeDir.ensure(env(null, h7.toString()), h7.toString());
        T.eq("rwx------", perms(run7), "an open ~/.fluxdm/run of ours is closed");

        Path h8 = fresh("home");
        Files.createDirectories(h8.resolve(".fluxdm"));
        Path target = fresh("elsewhere");
        String targetBefore = perms(target);
        Files.createSymbolicLink(h8.resolve(".fluxdm/run"), target);
        Throwable viaLink = thrown(() -> RuntimeDir.ensure(env(null, h8.toString()), h8.toString()));
        T.ok(viaLink instanceof IOException && says(viaLink, "not a plain directory"), "a symbolic link in its place is refused (" + viaLink + ")");
        T.eq(targetBefore, perms(target), "...and what it pointed at was left alone");

        Path h9 = fresh("home");
        Files.createDirectories(h9.resolve(".fluxdm"));
        Files.createFile(h9.resolve(".fluxdm/run"));
        Throwable viaFile = thrown(() -> RuntimeDir.ensure(env(null, h9.toString()), h9.toString()));
        T.ok(viaFile instanceof IOException && says(viaFile, "not a plain directory"), "a file in its place is refused (" + viaFile + ")");

        /* A path that cannot fit sockaddr_un is found out before anything is created. */
        Path tooLong = root.resolve("x".repeat(100));
        Throwable longErr = thrown(() -> RuntimeDir.ensure(env(null, tooLong.toString()), tooLong.toString()));
        T.ok(longErr instanceof IOException && says(longErr, "too long"), "a socket path over 107 bytes is refused (" + longErr + ")");
        T.ok(!Files.exists(tooLong), "...before any directory was created");
        if ("UTF-8".equalsIgnoreCase(System.getProperty("sun.jnu.encoding"))) {
            String wide = "/" + "é".repeat(50);                  // 51 characters, 101 bytes
            Throwable wideErr = thrown(() -> RuntimeDir.ensure(env(null, wide), wide));
            T.ok(wideErr instanceof IOException && says(wideErr, "too long"), "the limit counts bytes, not characters (" + wideErr + ")");
        } else {
            note("the file-name encoding is not UTF-8: the multi-byte path case is skipped");
        }

        if (isRoot() && lookup("nobody") != null) {
            UserPrincipal nobody = lookup("nobody");
            Path hx = fresh("home"), xx = fresh("xdg");
            chmod(xx, "rwx------");
            Files.setOwner(xx, nobody);
            T.eq(hx.resolve(".fluxdm/run"), choose(env(xx.toString(), hx.toString()), hx.toString()).dir(),
                 "a $XDG_RUNTIME_DIR that belongs to somebody else is not trusted");
            Path hy = fresh("home");
            Path runy = Files.createDirectories(hy.resolve(".fluxdm/run"));
            Files.setOwner(runy, nobody);
            Throwable foreign = thrown(() -> RuntimeDir.ensure(env(null, hy.toString()), hy.toString()));
            T.ok(foreign instanceof IOException && says(foreign, "belongs to another user"), "somebody else's ~/.fluxdm/run is refused (" + foreign + ")");
        } else {
            note("not root (or no 'nobody' account): the foreign-owner cases are skipped");
        }
    }

    /* ═══ ApiToken ═══ */

    static String read(Path p) throws IOException { return Files.readString(p, StandardCharsets.US_ASCII); }

    static void apiToken() throws Exception {
        T.section("API token file: created private, kept, replaced when it is no longer secret");

        Path dir = fresh("tok");
        Path file = dir.resolve("sub/api-token");
        ApiToken t = ApiToken.loadOrCreate(file);
        String text = read(file);
        T.eq(file, t.file(), "the token lives where it was asked to (missing directories are created)");
        T.eq("rw-------", perms(file), "the file is mode 0600");
        T.eq("rwx------", perms(file.getParent()), "...and a directory made for it is mode 0700");
        T.ok(text.matches("[A-Za-z0-9_-]{43}"), "it holds 43 URL-safe characters = 256 bits, no newline (" + text.length() + " chars)");
        try (Stream<Path> s = Files.list(file.getParent())) {
            T.eq(List.of("api-token"), s.map(p -> p.getFileName().toString()).toList(), "no temporary file is left behind");
        }
        T.ok(t.matches(text), "the token matches itself");
        T.ok(!t.matches(text + "x") && !t.matches(text.substring(1)) && !t.matches(text.substring(0, 42))
             && !t.matches("") && !t.matches(null) && !t.matches(text.toUpperCase()) && !t.matches(" " + text),
             "...and nothing near it: longer, shorter, empty, null, other case, padded");
        T.ok(!t.matches("é".repeat(43)), "...nor a non-ASCII string of the same length");

        ApiToken again = ApiToken.loadOrCreate(file);
        T.ok(again.matches(text) && read(file).equals(text), "loading again keeps the same token (clients can cache it)");

        Set<String> seen = new HashSet<>();
        boolean allMatch = true;
        for (int i = 0; i < 200; i++) {
            Path f = dir.resolve("many-" + i);
            ApiToken x = ApiToken.loadOrCreate(f);
            allMatch &= x.matches(read(f));
            seen.add(read(f));
        }
        T.ok(allMatch, "each new token matches its own file");
        T.eq(200, seen.size(), "200 new tokens are all different");

        /* No longer secret: replaced. */
        for (String mode : new String[] {"rw-r--r--", "rw-r-----", "rw----r--", "rw-rw----", "rwxrwxrwx"}) {
            Path f = dir.resolve("open-" + mode);
            ApiToken old = ApiToken.loadOrCreate(f);
            String oldText = read(f);
            chmod(f, mode);
            String log = stderrOf(() -> ApiToken.loadOrCreate(f));
            String newText = read(f);
            T.ok(!newText.equals(oldText) && newText.matches("[A-Za-z0-9_-]{43}"), "a token file with mode " + mode + " is replaced by a new token");
            T.eq("rw-------", perms(f), "...which is private");
            T.ok(log.contains("no longer secret"), "...and the log says why");
            T.ok(old.matches(oldText) && !ApiToken.loadOrCreate(f).matches(oldText), "...so the old token stops working");
        }
        Path exec = dir.resolve("owner-exec");
        ApiToken keep = ApiToken.loadOrCreate(exec);
        String keepText = read(exec);
        chmod(exec, "rwx------");
        T.ok(ApiToken.loadOrCreate(exec).matches(keepText) && keep.matches(keepText), "owner-only bits other than rw do not matter: the token is kept");

        /* Not usable as a token: replaced. */
        String good = "0123456789abcdef0123456789abcdef";
        String[][] cases = {
            {"empty",         ""},
            {"too short",     "hunter2"},
            {"21 characters", "a".repeat(21)},
            {"has a space",   "abcdefghijklmnopqrstuvwxyz abcdef"},
            {"has a quote",   "abcdefghijklmnopqrstuvwxyz\"abcdef"},
            {"is not ASCII",  "é".repeat(40)},
            {"is huge",       "a".repeat(5000)},
            {"257 characters", "a".repeat(257)},
        };
        for (String[] c : cases) {
            Path f = dir.resolve("bad-" + c[0].replace(' ', '-'));
            Files.writeString(f, c[1], StandardCharsets.UTF_8);
            chmod(f, "rw-------");
            String log = stderrOf(() -> ApiToken.loadOrCreate(f));
            T.ok(read(f).matches("[A-Za-z0-9_-]{43}") && !read(f).equals(c[1]) && log.contains("usable token"),
                 "a file whose content " + c[0] + " is replaced (and the log says so)");
        }
        Path own = dir.resolve("own-token");
        Files.writeString(own, good + "\n");
        chmod(own, "rw-------");
        ApiToken mine = ApiToken.loadOrCreate(own);
        T.ok(mine.matches(good) && read(own).equals(good + "\n"), "a token you made yourself (a trailing newline and all) is used as it is");
        Path hex = dir.resolve("hex-token");
        String hex64 = "9f".repeat(32);
        Files.writeString(hex, hex64);
        chmod(hex, "rw-------");
        T.ok(ApiToken.loadOrCreate(hex).matches(hex64), "so is a 64-character hex token");

        /* Refused outright. */
        Path realFile = dir.resolve("real");
        Files.writeString(realFile, good);
        chmod(realFile, "rw-------");
        Path link = dir.resolve("link-token");
        Files.createSymbolicLink(link, realFile);
        Throwable viaLink = thrown(() -> ApiToken.loadOrCreate(link));
        T.ok(viaLink instanceof IOException && says(viaLink, "not a regular file"), "a symbolic link is refused, not followed (" + viaLink + ")");
        T.eq(good, read(realFile), "...and its target is untouched");
        Path asDir = Files.createDirectory(dir.resolve("a-directory"));
        Throwable viaDir = thrown(() -> ApiToken.loadOrCreate(asDir));
        T.ok(viaDir instanceof IOException && says(viaDir, "not a regular file"), "a directory is refused (" + viaDir + ")");
        if (isRoot() && lookup("nobody") != null) {
            Path theirs = dir.resolve("theirs");
            Files.writeString(theirs, good);
            chmod(theirs, "rw-------");
            Files.setOwner(theirs, lookup("nobody"));
            Throwable foreign = thrown(() -> ApiToken.loadOrCreate(theirs));
            T.ok(foreign instanceof IOException && says(foreign, "belongs to another user"), "somebody else's file is refused (" + foreign + ")");
        } else {
            note("not root (or no 'nobody' account): the foreign-owner case is skipped");
        }
        if (!isRoot()) {
            Path ro = fresh("readonly");
            chmod(ro, "r-xr-xr-x");
            Throwable denied = thrown(() -> ApiToken.loadOrCreate(ro.resolve("api-token")));
            T.ok(denied instanceof IOException, "a directory that cannot be written is an error, not a silent unprotected API (" + denied + ")");
            chmod(ro, "rwx------");
        } else {
            note("running as root: the unwritable-directory case is skipped");
        }
    }

    /* ═══ Host / Origin / token decisions ═══ */

    static ApiGuard guard(boolean auth, String hosts, String origins, Path tokenFile) {
        return new ApiGuard(auth, tokenFile == null ? "" : tokenFile.toString(), hosts, origins);
    }

    static ApiGuard defaultGuard() throws IOException {
        Path f = fresh("guard").resolve("api-token");
        return guard(true, ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS, f);
    }

    static String tokenOf(ApiGuard g) throws IOException { return read(g.tokenFile()); }

    static void decisions() throws Exception {
        T.section("Decisions: Host, then Origin, then token");
        ApiGuard g = defaultGuard();
        String tok = "Bearer " + tokenOf(g);
        String ok = "127.0.0.1:7171";

        for (String host : new String[] {"127.0.0.1:7171", "127.0.0.1", "localhost", "localhost:7171", "LocalHost:7171",
                                         "[::1]:7171", "[::1]", " localhost:7171 ", "localhost:1", "localhost:65535"}) {
            T.eq(Verdict.ALLOWED, g.check(host, null, tok, true), "Host \"" + host + "\" is allowed");
        }
        for (String host : new String[] {null, "", "  ", "evil.example", "evil.example:7171", "127.0.0.1.evil.example",
                                         "localhost.evil.example:7171", "evil-localhost", "xlocalhost", "localhostx",
                                         "127.0.0.1@evil.example", "evil.example:80@127.0.0.1", "localhost:", "localhost:abc",
                                         "localhost:123456", "localhost:-1", "[::1", "[::1]x", "[::1]:", "::1", "0.0.0.0",
                                         "127.0.0.2", "127.1", "2130706433", "0x7f.0.0.1", "localhost.", "local host",
                                         "localhost,evil.example", "localhost\r\nX: y", "[::2]:7171", "localhost:7171:7171"}) {
            T.eq(Verdict.BAD_HOST, g.check(host, null, tok, true), "Host " + (host == null ? "(absent)" : "\"" + host.replace("\r", "\\r").replace("\n", "\\n") + "\"") + " is refused");
        }

        for (String origin : new String[] {null, "http://localhost:3000", "app://-"}) {
            T.eq(Verdict.ALLOWED, g.check(ok, origin, tok, true), "Origin " + origin + " is allowed");
        }
        for (String origin : new String[] {"null", "", "http://evil.example", "http://localhost:3001", "https://localhost:3000",
                                           "http://localhost:3000/", "HTTP://LOCALHOST:3000", "http://localhost:3000.evil.example",
                                           "http://evil.example/http://localhost:3000", "file://", "app://", "app://-/x",
                                           "http://127.0.0.1:3000", "chrome-extension://abcdefghijklmnop", "*"}) {
            T.eq(Verdict.BAD_ORIGIN, g.check(ok, origin, tok, true), "Origin \"" + origin + "\" is refused");
        }

        for (String auth : new String[] {tok, "bearer " + tokenOf(g), "BEARER " + tokenOf(g), "Bearer   " + tokenOf(g) + "  "}) {
            T.eq(Verdict.ALLOWED, g.check(ok, null, auth, true), "Authorization \"" + auth.substring(0, 11) + "...\" is accepted");
        }
        String t0 = tokenOf(g);
        for (String auth : new String[] {null, "", "Bearer", "Bearer ", "Bearer wrong", "Bearer " + t0 + "x", "Bearer " + t0.substring(1),
                                         "Basic " + t0, t0, "Token " + t0, "Bearer" + t0, "Bearer " + t0.toUpperCase(),
                                         "Bearer " + t0 + " " + t0, "Bearer \"" + t0 + "\"", " Bearer " + t0}) {
            T.eq(Verdict.UNAUTHORIZED, g.check(ok, null, auth, true), "Authorization " + (auth == null ? "(absent)" : "\"" + (auth.length() > 14 ? auth.substring(0, 14) + "..." : auth) + "\"") + " is refused");
        }

        T.eq(Verdict.BAD_HOST,   g.check("evil.example", "http://evil.example", null, true), "wrong host, wrong origin and no token: the host is what is reported");
        T.eq(Verdict.BAD_HOST,   g.check("evil.example", null, tok, true), "a valid token does not make a wrong host acceptable (DNS rebinding)");
        T.eq(Verdict.BAD_ORIGIN, g.check(ok, "http://evil.example", null, true), "right host, wrong origin and no token: the origin is what is reported");
        T.eq(Verdict.BAD_ORIGIN, g.check(ok, "http://evil.example", tok, true), "a valid token does not make a wrong origin acceptable");

        T.eq(Verdict.ALLOWED,    g.check(ok, "http://localhost:3000", null, false), "where no token can be sent (preflight, handshake) the token is not asked for");
        T.eq(Verdict.BAD_ORIGIN, g.check(ok, "http://evil.example", null, false), "...but the origin still is checked");
        T.eq(Verdict.BAD_HOST,   g.check("evil.example", null, null, false), "...and the host");

        T.ok(g.authorized(tok) && !g.authorized(null) && !g.authorized("Bearer nope") && !g.authorized("Basic " + t0), "authorized() looks at the token alone");
        T.ok(g.authEnabled() && g.tokenFile() != null, "authentication is on by default");
        T.eq(List.of("http://localhost:3000", "app://-"), Arrays.asList(g.allowedOrigins()), "allowedOrigins() lists the configured origins");
        String[] copy = g.allowedOrigins();
        copy[0] = "http://evil.example";
        T.eq(Verdict.ALLOWED, g.check(ok, "http://localhost:3000", tok, true), "...as a copy: changing it changes nothing");

        T.section("Verdicts");
        T.eq(403, Verdict.BAD_HOST.status(),     "a wrong host is 403");
        T.eq(403, Verdict.BAD_ORIGIN.status(),   "a wrong origin is 403");
        T.eq(401, Verdict.UNAUTHORIZED.status(), "a missing or wrong token is 401");
        for (Verdict v : Verdict.values()) {
            T.ok(v.message().matches("[A-Za-z ]+"), "the message for " + v + " is fixed text, nothing the client wrote (\"" + v.message() + "\")");
        }

        T.section("Log lines made from request headers");
        T.eq("a??b", ApiGuard.printable("a\r\nb"), "control characters cannot start a fake log line");
        T.eq("none", ApiGuard.printable(null), "an absent header prints as none");
        T.eq("a".repeat(100) + "...", ApiGuard.printable("a".repeat(300)), "a long value is cut");
        T.eq("?", ApiGuard.printable("é"), "non-ASCII becomes ?");
    }

    /* ═══ ApiGuard: setup and settings ═══ */

    static void guardSetup() throws Exception {
        T.section("Guard setup: settings, token file, refusing nonsense");

        Path d = fresh("setup");
        ApiGuard a = guard(true, " LocalHost , 127.0.0.1,localhost ,", " http://LOCALHOST:5173 ,APP://-,http://localhost:5173", d.resolve("api-token"));
        T.eq(Verdict.ALLOWED, a.check("127.0.0.1", "http://localhost:5173", "Bearer " + tokenOf(a), true), "lists tolerate spaces, case, duplicates and empty items");
        T.eq(List.of("http://localhost:5173", "app://-"), Arrays.asList(a.allowedOrigins()), "...and are stored lower-case without duplicates");

        Path d2 = fresh("setup");
        ApiGuard none = guard(true, "localhost", "", d2.resolve("api-token"));
        T.eq(0, none.allowedOrigins().length, "an empty origin list means no browser page may call the API");
        T.eq(Verdict.ALLOWED, none.check("localhost", null, "Bearer " + tokenOf(none), true), "...while programs that send no Origin still can");
        T.eq(Verdict.BAD_ORIGIN, none.check("localhost", "http://localhost:3000", "Bearer " + tokenOf(none), true), "...and every Origin is refused");

        for (String bad : new String[] {"*", "", " , ", "local*", "localhost:7171", "http://localhost", "-bad", "bad-", "a b", "[::1", "evil.example/x"}) {
            Throwable t = thrown(() -> guard(true, bad, ApiGuard.DEFAULT_ORIGINS, fresh("bad").resolve("api-token")));
            T.ok(t instanceof IllegalStateException && says(t, "fluxdm.api.allowed-hosts"), "allowed-hosts \"" + bad + "\" stops the daemon with a message naming the setting");
        }
        for (String bad : new String[] {"*", "http://*", "http://*.example.com", "http://localhost:3000/", "http://localhost:3000/x", "localhost:3000",
                                        "http://", "http://localhost:", "http://localhost:99999x", "http://local host", "null", "http://localhost?x"}) {
            Throwable t = thrown(() -> guard(true, ApiGuard.DEFAULT_HOSTS, bad, fresh("bad").resolve("api-token")));
            T.ok(t instanceof IllegalStateException && says(t, "fluxdm.api.allowed-origins"), "allowed-origins \"" + bad + "\" stops the daemon with a message naming the setting");
        }

        Path d3 = fresh("setup");
        Path blocked = Files.createDirectory(d3.resolve("api-token"));
        Throwable noToken = thrown(() -> guard(true, ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS, blocked));
        T.ok(noToken instanceof IllegalStateException && says(noToken, blocked.toString()) && says(noToken, "fluxdm.api.token-file"),
             "a token file that cannot be prepared stops the daemon, naming the file and the setting (" + noToken + ")");

        Path d4 = fresh("setup");
        Path never = d4.resolve("api-token");
        String warning = stderrOf(() -> {
            ApiGuard off = guard(false, ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS, never);
            T.ok(!off.authEnabled() && off.tokenFile() == null, "authentication switched off: no token in use");
            T.eq(Verdict.ALLOWED, off.check("localhost", null, null, true), "...requests need no token");
            T.ok(off.authorized(null) && off.authorized("Bearer whatever"), "...and the STOMP check passes too");
            T.eq(Verdict.BAD_HOST,   off.check("evil.example", null, null, true), "...but the host is still checked");
            T.eq(Verdict.BAD_ORIGIN, off.check("localhost", "http://evil.example", null, true), "...and the origin");
        });
        T.ok(!Files.exists(never), "...and no token file is created");
        T.ok(warning.contains("authentication is OFF"), "...and the log says so loudly");

        /* The default token file is ~/.fluxdm/api-token. */
        String oldHome = System.getProperty("user.home");
        Path fakeHome = fresh("userhome");
        try {
            System.setProperty("user.home", fakeHome.toString());
            ApiGuard dflt = new ApiGuard(true, "", ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS);
            T.eq(fakeHome.resolve(".fluxdm/api-token"), dflt.tokenFile(), "no token-file setting: the token is ~/.fluxdm/api-token");
            T.eq("rw-------", perms(dflt.tokenFile()), "...mode 0600");
            ApiGuard blank = new ApiGuard(true, "   ", ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS);
            T.eq(dflt.tokenFile(), blank.tokenFile(), "a blank setting means the same");
            T.eq(Verdict.ALLOWED, blank.check("localhost", null, "Bearer " + tokenOf(dflt), true), "...with the same token");
        } finally {
            System.setProperty("user.home", oldHome);
        }

        /* Refusals are logged, but a page that keeps trying cannot fill the log. */
        ApiGuard g = defaultGuard();
        String flood = stderrOf(() -> {
            for (int i = 0; i < 1000; i++) g.noteRejected(Verdict.BAD_HOST, "Host: evil.example");
            for (int i = 0; i < 1000; i++) g.noteRejected(Verdict.BAD_ORIGIN, "Origin: http://evil.example");
            for (int i = 0; i < 1000; i++) g.noteRejected(Verdict.UNAUTHORIZED, "GET /api/downloads");
        });
        T.eq(3L, count(flood, "refused a request"), "3000 refusals of three kinds are logged as 3 lines");
        T.ok(flood.contains("host not allowed") && flood.contains("origin not allowed") && flood.contains("missing or invalid API token"), "...one for each kind");
    }

    /* ═══ HTTP filter, driven through stand-in servlet objects ═══ */

    static final class Exchange {
        final Map<String, String>       in  = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        final Map<String, List<String>> out = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        final StringWriter              body = new StringWriter();
        String  method = "GET", uri = "/api/downloads", contentType;
        int     status = 200;
        boolean chainCalled;

        Exchange(String method, String uri) { this.method = method; this.uri = uri; }

        Exchange header(String name, String value) { in.put(name, value); return this; }

        Exchange host(String v)   { return header("Host", v); }
        Exchange origin(String v) { return header("Origin", v); }
        Exchange token(String v)  { return header("Authorization", "Bearer " + v); }

        String sent(String name) { List<String> v = out.get(name); return v == null ? null : String.join(",", v); }

        static Object blank(Method m) {
            Class<?> r = m.getReturnType();
            if (r == boolean.class) return false;
            if (r == int.class)     return 0;
            if (r == long.class)    return 0L;
            return null;
        }

        HttpServletRequest request() {
            return (HttpServletRequest) Proxy.newProxyInstance(ApiSecurityTest.class.getClassLoader(),
                new Class<?>[] {HttpServletRequest.class}, (p, m, a) -> switch (m.getName()) {
                    case "getHeader"     -> in.get((String) a[0]);
                    case "getMethod"     -> method;
                    case "getRequestURI" -> uri;
                    default              -> blank(m);
                });
        }

        HttpServletResponse response() {
            return (HttpServletResponse) Proxy.newProxyInstance(ApiSecurityTest.class.getClassLoader(),
                new Class<?>[] {HttpServletResponse.class}, (p, m, a) -> {
                    switch (m.getName()) {
                        case "setStatus"            -> status = (Integer) a[0];
                        case "setContentType"       -> contentType = (String) a[0];
                        case "setHeader"            -> out.put((String) a[0], new ArrayList<>(List.of((String) a[1])));
                        case "addHeader"            -> out.computeIfAbsent((String) a[0], k -> new ArrayList<>()).add((String) a[1]);
                        case "getWriter"            -> { return new PrintWriter(body, true); }
                        default                     -> { /* setCharacterEncoding and the rest: nothing to record */ }
                    }
                    return blank(m);
                });
        }

        Exchange through(ApiGuardFilter f) throws Exception {
            f.doFilter(request(), response(), (rq, rs) -> chainCalled = true);
            return this;
        }

        boolean passed() { return chainCalled && status == 200 && body.toString().isEmpty(); }
    }

    static void httpFilter() throws Exception {
        T.section("HTTP filter in front of Spring MVC");
        ApiGuard g = defaultGuard();
        ApiGuardFilter f = new ApiGuardFilter(g);
        String tok = tokenOf(g);

        T.ok(new Exchange("GET", "/api/downloads").host("127.0.0.1:7171").token(tok).through(f).passed(), "a request with the right host and token goes through");
        T.ok(new Exchange("POST", "/api/downloads").host("localhost:7171").origin("http://localhost:3000").token(tok).through(f).passed(), "...also from the allowed web origin");

        Exchange noTok = new Exchange("GET", "/api/downloads").host("127.0.0.1:7171").through(f);
        T.ok(!noTok.chainCalled && noTok.status == 401, "no token: 401, and Spring never sees the request");
        T.eq("{\"error\":\"missing or invalid API token\"}", noTok.body.toString(), "...with a small JSON error body");
        T.eq("application/json", noTok.contentType, "...of type application/json");
        T.eq("Bearer realm=\"fluxdm\"", noTok.sent("WWW-Authenticate"), "...naming the scheme the API wants");
        T.eq("no-store", noTok.sent("Cache-Control"), "...that must not be cached");
        T.eq(null, noTok.sent("Access-Control-Allow-Origin"), "...and carries no CORS header when the request had no origin");

        Exchange wrong = new Exchange("GET", "/api/downloads").host("127.0.0.1:7171").token("wrong").through(f);
        T.ok(!wrong.chainCalled && wrong.status == 401, "a wrong token: 401");

        Exchange rebind = new Exchange("GET", "/api/downloads").host("evil.example:7171").token(tok).through(f);
        T.ok(!rebind.chainCalled && rebind.status == 403, "DNS rebinding: a request that names another host is 403 even with the token");
        T.eq("{\"error\":\"host not allowed\"}", rebind.body.toString(), "...and the body does not repeat what the client sent");
        T.eq(null, rebind.sent("WWW-Authenticate"), "...and does not invite a retry with credentials");
        Exchange rebindNoHost = new Exchange("GET", "/api/health").through(f);
        T.ok(!rebindNoHost.chainCalled && rebindNoHost.status == 403, "a request with no Host header at all is 403");

        Exchange csrf = new Exchange("POST", "/api/downloads/5/pause").host("localhost:7171").origin("http://evil.example")
                .header("Content-Type", "text/plain").through(f);
        T.ok(!csrf.chainCalled && csrf.status == 403, "cross-site simple POST from another web page: 403");
        T.eq("{\"error\":\"origin not allowed\"}", csrf.body.toString(), "...with a fixed body");
        T.eq(null, csrf.sent("Access-Control-Allow-Origin"), "...and no CORS header, so the page cannot read even the refusal");
        Exchange csrfToken = new Exchange("DELETE", "/api/downloads/5").host("localhost:7171").origin("http://evil.example").token(tok).through(f);
        T.ok(!csrfToken.chainCalled && csrfToken.status == 403, "another origin is refused even with the token");
        Exchange nullOrigin = new Exchange("POST", "/api/downloads").host("localhost:7171").origin("null").token(tok).through(f);
        T.ok(!nullOrigin.chainCalled && nullOrigin.status == 403, "\"Origin: null\" (sandboxed frames, data: pages) is refused");

        Exchange page = new Exchange("GET", "/api/downloads").host("localhost:7171").origin("http://localhost:3000").through(f);
        T.ok(!page.chainCalled && page.status == 401, "the allowed web origin without a token: 401");
        T.eq("http://localhost:3000", page.sent("Access-Control-Allow-Origin"), "...which its page may read (so the UI can say \"token needed\")");
        T.eq("Origin", page.sent("Vary"), "...with Vary: Origin");

        /* CORS preflights cannot carry the token. */
        Exchange pre = new Exchange("OPTIONS", "/api/downloads").host("localhost:7171").origin("http://localhost:3000")
                .header("Access-Control-Request-Method", "POST").header("Access-Control-Request-Headers", "authorization,content-type").through(f);
        T.ok(pre.passed(), "a CORS preflight from an allowed origin goes through without a token (Spring answers it)");
        Exchange preEvil = new Exchange("OPTIONS", "/api/downloads").host("localhost:7171").origin("http://evil.example")
                .header("Access-Control-Request-Method", "POST").through(f);
        T.ok(!preEvil.chainCalled && preEvil.status == 403, "a preflight from another origin: 403");
        Exchange preHost = new Exchange("OPTIONS", "/api/downloads").host("evil.example").origin("http://localhost:3000")
                .header("Access-Control-Request-Method", "POST").through(f);
        T.ok(!preHost.chainCalled && preHost.status == 403, "a preflight that names another host: 403");
        Exchange plainOptions = new Exchange("OPTIONS", "/api/downloads").host("localhost:7171").origin("http://localhost:3000").through(f);
        T.ok(!plainOptions.chainCalled && plainOptions.status == 401, "OPTIONS without Access-Control-Request-Method is not a preflight: token needed");
        Exchange noOriginOptions = new Exchange("OPTIONS", "/api/downloads").host("localhost:7171").header("Access-Control-Request-Method", "POST").through(f);
        T.ok(!noOriginOptions.chainCalled && noOriginOptions.status == 401, "...nor is OPTIONS without an Origin");
        T.ok(new Exchange("options", "/api/downloads").host("localhost:7171").origin("http://localhost:3000").header("Access-Control-Request-Method", "GET").through(f).passed(),
             "the method name is compared without regard to case");
        Exchange getWithAcrm = new Exchange("GET", "/api/downloads").host("localhost:7171").origin("http://localhost:3000").header("Access-Control-Request-Method", "GET").through(f);
        T.ok(!getWithAcrm.chainCalled && getWithAcrm.status == 401, "only OPTIONS can be a preflight: a GET that claims to be one needs the token");

        /* The WebSocket endpoint takes its token in the STOMP CONNECT frame, but is open to nobody's web page. */
        for (String path : new String[] {"/ws", "/ws/info", "/ws/websocket", "/ws/iframe.html", "/ws/123/abcdefgh/websocket", "/ws/000/a_b-c/xhr_streaming"}) {
            T.ok(new Exchange("GET", path).host("localhost:7171").origin("http://localhost:3000").through(f).passed(), path + " does not need the token at the HTTP layer");
        }
        Exchange wsEvil = new Exchange("GET", "/ws/info").host("localhost:7171").origin("http://evil.example").through(f);
        T.ok(!wsEvil.chainCalled && wsEvil.status == 403, "/ws/info from another origin: 403");
        Exchange wsHost = new Exchange("GET", "/ws/websocket").host("evil.example:7171").through(f);
        T.ok(!wsHost.chainCalled && wsHost.status == 403, "/ws/websocket with another host: 403");
        for (String uri : new String[] {"/ws/../api/downloads", "/ws/./info", "/ws/%2e%2e/api/downloads", "/ws/%2E%2E/api/downloads", "/ws/..%2fapi/downloads",
                                        "/ws//info", "/ws/", "/wsx", "/ws.json", "/ws;jsessionid=1", "/ws/info;x=1", "/WS/info", "/api/ws", "/api/ws/info", "/x/ws",
                                        "/ws/info/../../api/downloads", "/ws/\u2025/api", "/ws/a b", "/ws/info\r\nX: y", ""}) {
            Exchange x = new Exchange("GET", uri).host("localhost:7171").origin("http://localhost:3000").through(f);
            T.ok(!x.chainCalled && x.status == 401, "\"" + uri.replace("\r", "\\r").replace("\n", "\\n") + "\" is not the WebSocket endpoint: token needed");
        }

        /* The log. */
        ApiGuardFilter f2 = new ApiGuardFilter(defaultGuard());
        String log = stderrOf(() -> {
            for (int i = 0; i < 500; i++) new Exchange("GET", "/api/downloads").host("evil.example").through(f2);
            new Exchange("GET", "/api/downloads").host("evil.example\r\nINJECTED: yes").through(new ApiGuardFilter(defaultGuard()));
        });
        T.eq(2L, count(log, "refused a request"), "500 refused requests are two log lines here (one per guard), not 501");
        T.ok(Arrays.stream(log.split("\n")).noneMatch(l -> l.startsWith("INJECTED")), "a header cannot forge a log line");
        T.ok(log.contains("Host: evil.example"), "the log names what was refused");

        /* Authentication off: Host and Origin still apply. */
        Path d = fresh("noauth");
        ApiGuardFilter off = new ApiGuardFilter(guard(false, ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS, d.resolve("api-token")));
        T.ok(new Exchange("GET", "/api/downloads").host("localhost:7171").through(off).passed(), "auth off: a request without a token goes through");
        T.ok(new Exchange("GET", "/api/downloads").host("evil.example").through(off).status == 403, "auth off: another host is still refused");
        T.ok(new Exchange("POST", "/api/downloads").host("localhost").origin("http://evil.example").through(off).status == 403, "auth off: another origin is still refused");
    }

    /* ═══ STOMP interceptor ═══ */

    record Frame(StompHeaderAccessor accessor) implements Message<byte[]> {
        @Override public byte[] getPayload() { return new byte[0]; }
        @Override public MessageHeaders getHeaders() {
            MessageHeaders h = new MessageHeaders();
            if (accessor != null) h.put(MessageHeaderAccessor.ACCESSOR_KEY, accessor);
            return h;
        }
    }

    /** Plays the part of Spring's STOMP session handler: after a CONNECT that set a user, every later frame of the
     *  session carries that user. */
    static final class Session {
        final StompAuthInterceptor interceptor;
        Principal user;

        Session(ApiGuard g) { this.interceptor = new StompAuthInterceptor(g); }

        Message<?> send(StompCommand command, String authorization) {
            StompHeaderAccessor a = StompHeaderAccessor.create(command);
            if (authorization != null) a.setNativeHeader("Authorization", authorization);
            a.setUser(user);
            Message<?> out = interceptor.preSend(new Frame(a), null);
            if (command == StompCommand.CONNECT || command == StompCommand.STOMP) user = a.getUser();
            return out;
        }

        boolean accepts(StompCommand command, String authorization) {
            try { return send(command, authorization) != null; } catch (MessagingException e) { return false; }
        }
    }

    static void stompInterceptor() throws Exception {
        T.section("STOMP: the token is required in the CONNECT frame");
        ApiGuard g = defaultGuard();
        String tok = "Bearer " + tokenOf(g);

        Session s = new Session(g);
        T.ok(!s.accepts(StompCommand.SUBSCRIBE, null),  "SUBSCRIBE before CONNECT is refused");
        T.ok(!s.accepts(StompCommand.SEND, null),       "SEND before CONNECT is refused");
        T.ok(!s.accepts(StompCommand.UNSUBSCRIBE, tok), "...even if it carries the token itself: only CONNECT authenticates");
        T.ok(!s.accepts(StompCommand.CONNECT, null),    "CONNECT without an Authorization header is refused");
        T.ok(!s.accepts(StompCommand.CONNECT, "Bearer wrong"), "CONNECT with the wrong token is refused");
        T.ok(!s.accepts(StompCommand.CONNECT, "Basic " + tokenOf(g)), "CONNECT with another scheme is refused");
        T.ok(!s.accepts(StompCommand.CONNECT, tokenOf(g)), "CONNECT with the bare token (no \"Bearer \") is refused");
        T.ok(!s.accepts(StompCommand.SUBSCRIBE, null),  "failed CONNECTs did not authenticate the session");
        Throwable why = thrown(() -> s.send(StompCommand.CONNECT, "Bearer wrong"));
        T.ok(why instanceof MessagingException && says(why, "API token"), "the refusal says why (" + why + ")");
        Throwable whyNot = thrown(() -> s.send(StompCommand.SEND, null));
        T.ok(whyNot instanceof MessagingException && says(whyNot, "CONNECT"), "...and what to do (" + whyNot + ")");
        T.ok(why.getStackTrace().length == 0 && whyNot.getStackTrace().length == 0,
             "...neither carries a stack trace, so the log entry Spring writes for a refused frame stays short");

        T.ok(s.accepts(StompCommand.CONNECT, tok), "CONNECT with the token is accepted");
        T.ok(s.user != null, "...and marks the session as authenticated");
        T.ok(s.accepts(StompCommand.SUBSCRIBE, null) && s.accepts(StompCommand.SEND, null) && s.accepts(StompCommand.UNSUBSCRIBE, null)
             && s.accepts(StompCommand.ACK, null) && s.accepts(StompCommand.BEGIN, null), "later frames of that session need nothing more");

        Session other = new Session(g);
        T.ok(!other.accepts(StompCommand.SUBSCRIBE, null), "another session is not authenticated by the first one");
        T.ok(other.accepts(StompCommand.STOMP, tok) && other.accepts(StompCommand.SUBSCRIBE, null), "the STOMP 1.2 spelling of CONNECT works the same");

        Session closing = new Session(g);
        T.ok(closing.accepts(StompCommand.DISCONNECT, null), "DISCONNECT is always allowed (a session that never got in must still be able to leave)");
        T.ok(closing.interceptor.preSend(new Frame(StompHeaderAccessor.create(null)), null) != null, "heartbeats pass");
        T.ok(closing.interceptor.preSend(new Frame(null), null) != null, "so does a message that has no STOMP header accessor at all");

        Session reconnect = new Session(g);
        T.ok(reconnect.accepts(StompCommand.CONNECT, tok), "a session connects...");
        T.ok(!reconnect.accepts(StompCommand.CONNECT, "Bearer wrong"), "...and a second CONNECT with a wrong token is still refused");

        Path d = fresh("stomp-off");
        Session off = new Session(guard(false, ApiGuard.DEFAULT_HOSTS, ApiGuard.DEFAULT_ORIGINS, d.resolve("api-token")));
        T.ok(off.accepts(StompCommand.SUBSCRIBE, null) && off.accepts(StompCommand.CONNECT, null), "auth off: frames pass without a token");
    }

    /* ═══ Wiring: CORS, WebSocket, and the property names ═══ */

    static Properties shipped() throws IOException {
        Path dir = Path.of(System.getProperty("fluxdm.root", "")).toAbsolutePath();
        for (int up = 0; up < 5 && dir != null; up++, dir = dir.getParent()) {
            Path f = dir.resolve("daemon/src/main/resources/application.properties");
            if (Files.isRegularFile(f)) {
                Properties p = new Properties();
                try (var in = Files.newBufferedReader(f)) { p.load(in); }
                return p;
            }
        }
        return null;
    }

    static void wiring() throws Exception {
        T.section("CORS and WebSocket take their origins from the same list");
        Path d = fresh("wiring");
        ApiGuard g = guard(true, ApiGuard.DEFAULT_HOSTS, "http://localhost:5173,app://-", d.resolve("api-token"));

        CorsRegistry cors = new CorsRegistry();
        new CorsConfig(g).addCorsMappings(cors);
        T.eq(1, cors.registrations.size(), "one CORS mapping");
        var c = cors.registrations.get(0);
        T.eq("/api/**", c.pathPattern, "...for /api/**");
        T.eq(List.of("http://localhost:5173", "app://-"), Arrays.asList(c.origins), "...with the configured origins and no wildcard");
        T.ok(Arrays.asList(c.methods).containsAll(List.of("GET", "POST", "PUT", "DELETE", "OPTIONS")), "...for the methods the API uses");
        T.ok(Arrays.asList(c.headers).containsAll(List.of("Authorization", "Content-Type")), "...allowing the Authorization header the token travels in");
        T.ok(c.maxAge > 0, "...and letting the browser cache the preflight answer");

        StompWebSocketEndpointRegistration[] reg = new StompWebSocketEndpointRegistration[1];
        String[][] paths = new String[1][];
        WebSocketConfig ws = new WebSocketConfig(g);
        ws.registerStompEndpoints(p -> { paths[0] = p; reg[0] = new StompWebSocketEndpointRegistration(); return reg[0]; });
        T.eq(List.of("/ws"), Arrays.asList(paths[0]), "the STOMP endpoint is /ws");
        T.eq(List.of("http://localhost:5173", "app://-"), Arrays.asList(reg[0].origins), "...open to the configured origins only");
        T.eq(0, reg[0].originPatterns.length, "...and not to the pattern \"*\" it used to be open to");
        T.ok(reg[0].sockJs, "...with the SockJS fallback");
        ChannelRegistration channel = new ChannelRegistration();
        ws.configureClientInboundChannel(channel);
        T.ok(channel.registered.size() == 1 && channel.registered.get(0) instanceof StompAuthInterceptor, "the CONNECT token check is on the inbound channel");

        T.section("Settings: application.properties and the @Value defaults agree");
        Properties p = shipped();
        if (p == null) {
            T.ok(false, "application.properties was not found (run from the repository, or give -Dfluxdm.root=<repo>)");
            return;
        }
        Pattern placeholder = Pattern.compile("\\$\\{([^:}]+):(.*)}");
        Map<String, String> defaults = new HashMap<>();
        for (Parameter prm : ApiGuard.class.getConstructors()[0].getParameters()) {
            Value v = prm.getAnnotation(Value.class);
            Matcher m = v == null ? null : placeholder.matcher(v.value());
            if (m == null || !m.matches()) { T.ok(false, "every ApiGuard constructor parameter has a ${name:default} @Value"); continue; }
            defaults.put(m.group(1), m.group(2));
        }
        T.eq(Set.of("fluxdm.api.auth.enabled", "fluxdm.api.token-file", "fluxdm.api.allowed-hosts", "fluxdm.api.allowed-origins"),
             defaults.keySet(), "ApiGuard reads exactly the four fluxdm.api.* settings");
        for (var e : defaults.entrySet()) {
            T.ok(p.containsKey(e.getKey()), e.getKey() + " is documented in application.properties");
            T.eq(e.getValue(), p.getProperty(e.getKey(), "").strip(), e.getKey() + ": the shipped value equals the built-in default");
        }
        T.eq(ApiGuard.DEFAULT_HOSTS,   defaults.get("fluxdm.api.allowed-hosts"),   "the built-in host default is the DEFAULT_HOSTS constant");
        T.eq(ApiGuard.DEFAULT_ORIGINS, defaults.get("fluxdm.api.allowed-origins"), "the built-in origin default is the DEFAULT_ORIGINS constant");

        String oldHome = System.getProperty("user.home");
        try {
            System.setProperty("user.home", fresh("userhome").toString());
            ApiGuard shippedGuard = new ApiGuard(Boolean.parseBoolean(p.getProperty("fluxdm.api.auth.enabled")), p.getProperty("fluxdm.api.token-file"),
                                                 p.getProperty("fluxdm.api.allowed-hosts"), p.getProperty("fluxdm.api.allowed-origins"));
            T.ok(shippedGuard.authEnabled(), "a guard built from the shipped settings has authentication on");
            T.eq(Verdict.ALLOWED, shippedGuard.check("127.0.0.1:7171", "http://localhost:3000", "Bearer " + tokenOf(shippedGuard), true), "...and accepts the UI's origin with the token");
        } finally {
            System.setProperty("user.home", oldHome);
        }

        String socket = p.getProperty("fluxdm.core.socket-path");
        T.ok(socket == null || !socket.startsWith("/tmp"), "the shipped settings no longer put the socket in /tmp (" + socket + ")");
        Value sockValue = com.fluxdm.ipc.CoreIPCClient.class.getConstructors()[0].getParameters()[0].getAnnotation(Value.class);
        T.eq("${fluxdm.core.socket-path:}", sockValue == null ? null : sockValue.value(), "CoreIPCClient's default socket path is \"\" = the private runtime directory");
        Set<String> keys = new HashSet<>(p.stringPropertyNames());
        T.ok(keys.stream().noneMatch(k -> k.startsWith("fluxdm.api.") && !defaults.containsKey(k)), "no fluxdm.api.* setting is left unread by the code");
    }
}
