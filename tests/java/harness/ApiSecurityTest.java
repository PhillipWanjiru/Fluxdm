/* FluxDM — tests/java/harness/ApiSecurityTest.java
 * The defences of the local API. This part: the private runtime directory the IPC socket lives in, and the
 * settings that point there. (The token, the Host / Origin checks and the WebSocket check follow.)
 *
 * Everything here runs the real classes of daemon/src/main. tests/java/harness/DaemonE2ETest.java adds the
 * cross-check against the real fluxdm-core: both must choose the same default socket. */
package harness;

import com.fluxdm.security.RuntimeDir;
import org.springframework.beans.factory.annotation.Value;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.stream.Stream;

public class ApiSecurityTest {

    static Path root;
    static int  seq;

    public static void main(String[] args) throws Exception {
        System.out.println("\n FluxDM — local API security tests");
        root = scratchRoot();
        try {
            quietly(ApiSecurityTest::runtimeDir);
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

    /* ═══ Settings ═══ */

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
        T.section("Settings: the socket is no longer put in /tmp");
        Properties p = shipped();
        if (p == null) {
            T.ok(false, "application.properties was not found (run from the repository, or give -Dfluxdm.root=<repo>)");
            return;
        }
        String socket = p.getProperty("fluxdm.core.socket-path");
        T.ok(socket == null || !socket.startsWith("/tmp"), "the shipped settings no longer put the socket in /tmp (" + socket + ")");
        Value sockValue = com.fluxdm.ipc.CoreIPCClient.class.getConstructors()[0].getParameters()[0].getAnnotation(Value.class);
        T.eq("${fluxdm.core.socket-path:}", sockValue == null ? null : sockValue.value(), "CoreIPCClient's default socket path is \"\" = the private runtime directory");
    }
}
