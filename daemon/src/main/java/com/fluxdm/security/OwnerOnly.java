/* FluxDM — security/OwnerOnly.java
 * The few file-permission facts the security package keeps asking: who am I, and is this open to anyone else?
 */
package com.fluxdm.security;

import java.io.IOException;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.util.Set;

final class OwnerOnly {
    private OwnerOnly() {}

    /** rwx------ : a directory only its owner can enter. */
    static final Set<PosixFilePermission> DIR  = PosixFilePermissions.fromString("rwx------");
    /** rw------- : a file only its owner can read. */
    static final Set<PosixFilePermission> FILE = PosixFilePermissions.fromString("rw-------");

    private static final Set<PosixFilePermission> OTHERS = PosixFilePermissions.fromString("---rwxrwx");

    /** True if the permissions leave anything to the group or to everybody else. */
    static boolean opensToOthers(Set<PosixFilePermission> perms) {
        for (PosixFilePermission p : perms) if (OTHERS.contains(p)) return true;
        return false;
    }

    /** The user this process runs as. The owner of /proc/self is the process's user; the user name is the fallback. */
    static UserPrincipal self() throws IOException {
        try {
            return Files.getOwner(Path.of("/proc/self"));
        } catch (IOException | UnsupportedOperationException | InvalidPathException e) {
            try {
                return FileSystems.getDefault().getUserPrincipalLookupService()
                                  .lookupPrincipalByName(System.getProperty("user.name"));
            } catch (IOException e2) {
                throw new IOException("cannot tell which user this process runs as: " + e2.getMessage(), e2);
            }
        }
    }
}
