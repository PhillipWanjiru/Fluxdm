/* FluxDM — security/RuntimeDir.java
 * The private directory that holds the IPC socket.
 *
 * Whoever can create the socket path first can answer the daemon, and in /tmp another user can. So the socket
 * lives in a directory only this user can enter:
 *
 *     $XDG_RUNTIME_DIR/fluxdm    when $XDG_RUNTIME_DIR is an absolute path to a directory that is ours and
 *                                closed to everyone else (systemd creates it that way)
 *     ~/.fluxdm/run              otherwise
 *
 * The core follows the same rules (core/src/ipc/sock_path.c). The directory is created with mode 0700. One of
 * ours that is too open is closed again; a symbolic link, a file or somebody else's directory is refused.
 */
package com.fluxdm.security;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.util.Map;

public final class RuntimeDir {
    private static final Logger log = LoggerFactory.getLogger(RuntimeDir.class);

    public static final String SOCKET_NAME = "core.sock";

    /** sizeof(sockaddr_un.sun_path): a socket path must be shorter than this many bytes. */
    static final int SUN_PATH_BYTES = 108;

    private RuntimeDir() {}

    /** The directory for this process's environment; created if it does not exist. */
    public static Path ensure() throws IOException {
        return ensure(System.getenv(), System.getProperty("user.home"));
    }

    /** The same decision from explicit inputs, so the rules can be tested without touching the environment. */
    public static Path ensure(Map<String, String> env, String userHome) throws IOException {
        UserPrincipal me  = OwnerOnly.self();
        String        xdg = env.get("XDG_RUNTIME_DIR");
        Path          dir;
        Path          base = null;                      // ~/.fluxdm, when that is where we end up

        if (isClosedDirOf(me, xdg)) {
            dir = Path.of(xdg).resolve("fluxdm");
        } else {
            if (xdg != null && !xdg.isEmpty()) {
                log.warn("XDG_RUNTIME_DIR ({}) is not an absolute path to a directory of yours that is closed to "
                       + "others; using ~/.fluxdm/run instead", xdg);
            }
            String home = env.get("HOME");
            if (home == null || !home.startsWith("/")) home = userHome;
            if (home == null || !home.startsWith("/")) {
                throw new IOException("cannot tell where your home directory is; set fluxdm.core.socket-path");
            }
            base = Path.of(home, ".fluxdm");
            dir  = base.resolve("run");
        }

        /* The core's socket goes in here, and a socket path has to fit sockaddr_un: find out before creating anything. */
        int need = (dir + "/" + SOCKET_NAME).getBytes(StandardCharsets.UTF_8).length;
        if (need >= SUN_PATH_BYTES) {
            throw new IOException("the default socket path would be too long (max " + (SUN_PATH_BYTES - 1)
                                  + " bytes): " + dir + "/" + SOCKET_NAME + "; set fluxdm.core.socket-path");
        }

        if (base != null) DataDir.ensurePrivate(base);         // the daemon keeps its data there, too
        makePrivate(dir, me);
        return dir;
    }

    /** {@code <runtime dir>/core.sock}, with the directory created. */
    public static Path defaultSocket() throws IOException {
        return ensure().resolve(SOCKET_NAME);
    }

    /** An absolute path to a directory that is ours and that nobody else can enter. A final symbolic link is
     *  followed, as stat(2) does: a runtime directory may legitimately be one. */
    private static boolean isClosedDirOf(UserPrincipal me, String path) {
        if (path == null || !path.startsWith("/")) return false;
        try {
            Path p = Path.of(path);
            return Files.isDirectory(p)
                && me.equals(Files.getOwner(p))
                && !OwnerOnly.opensToOthers(Files.getPosixFilePermissions(p));
        } catch (IOException | InvalidPathException | UnsupportedOperationException e) {
            return false;
        }
    }

    /** Create {@code dir} (mode 0700) unless it exists, then look at what is there. The parent must exist. */
    private static void makePrivate(Path dir, UserPrincipal me) throws IOException {
        try {
            Files.createDirectory(dir, PosixFilePermissions.asFileAttribute(OwnerOnly.DIR));
        } catch (FileAlreadyExistsException exists) {
            /* fine: it is inspected below */
        }
        BasicFileAttributes a = Files.readAttributes(dir, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        if (!a.isDirectory()) {
            throw new IOException(dir + " is not a plain directory (a symbolic link or a file?); remove it, "
                                  + "or set fluxdm.core.socket-path");
        }
        if (!me.equals(Files.getOwner(dir, LinkOption.NOFOLLOW_LINKS))) {
            throw new IOException(dir + " belongs to another user; remove it, or set fluxdm.core.socket-path");
        }
        if (OwnerOnly.opensToOthers(Files.getPosixFilePermissions(dir, LinkOption.NOFOLLOW_LINKS))) {
            Files.setPosixFilePermissions(dir, OwnerOnly.DIR);
        }
    }
}
