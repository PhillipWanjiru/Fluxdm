/* FluxDM — security/DataDir.java
 * ~/.fluxdm: where the daemon keeps its download database, its API token and, when $XDG_RUNTIME_DIR is not
 * private, the directory of the core's socket. Nothing in it is meant for other users (the database lists every
 * URL and destination path), so the directory is created with mode 0700, and one of ours that was left open, by an
 * older version or by a plain mkdir, is closed again.
 */
package com.fluxdm.security;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;

public final class DataDir {

    private DataDir() {}

    /**
     * Create {@code dir} (and any missing parents) with mode 0700, then close it if it belongs to this user and is
     * open to others. A final symbolic link is followed, as stat(2) does: a data directory may be a link to another
     * disk, and it is the directory it points at that is closed. A directory that belongs to somebody else is not ours
     * to change and is left as it is.
     *
     * @return true if an existing directory had to be closed
     * @throws IOException if the directory cannot be created or closed; the message says which
     */
    public static boolean ensurePrivate(Path dir) throws IOException {
        try {
            Files.createDirectories(dir, PosixFilePermissions.asFileAttribute(OwnerOnly.DIR));
        } catch (IOException e) {
            throw new IOException("could not create " + dir + ": " + describe(e), e);
        }
        try {
            if (!OwnerOnly.self().equals(Files.getOwner(dir))
                    || !OwnerOnly.opensToOthers(Files.getPosixFilePermissions(dir))) {
                return false;
            }
            Files.setPosixFilePermissions(dir, OwnerOnly.DIR);
            return true;
        } catch (IOException e) {
            throw new IOException("could not make " + dir + " private: " + describe(e), e);
        }
    }

    private static String describe(IOException e) {
        return e.getClass().getSimpleName() + (e.getMessage() == null ? "" : ": " + e.getMessage());
    }
}
