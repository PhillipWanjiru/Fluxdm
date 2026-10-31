/* FluxDM — security/ApiToken.java
 * The secret a client has to present to use the API.
 *
 * 256 random bits, kept in a file only this user can read (default ~/.fluxdm/api-token). Clients on the same
 * account read the file and send "Authorization: Bearer <token>". The token is stable across restarts so that
 * clients can keep it; to rotate it, delete the file and restart the daemon.
 *
 *  - The file is created atomically with mode 0600: written under a private temporary name, then renamed, so
 *    nobody ever reads a half-written token and the secret is never on disk with looser permissions.
 *  - A file that is ours but readable by others is no longer secret and is replaced; so is one that does not
 *    hold a usable token. A symbolic link, a directory or somebody else's file is refused.
 *  - Comparison is constant-time, so the response time says nothing about how much of a guess was right.
 */
package com.fluxdm.security;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.nio.file.attribute.UserPrincipal;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.regex.Pattern;

public final class ApiToken {
    private static final Logger log = LoggerFactory.getLogger(ApiToken.class);

    /** Random bytes in a token we generate: 256 bits, written as 43 URL-safe characters. */
    static final int BYTES = 32;

    /** What a token read from a file must look like: the RFC 6750 token68 characters, long enough to guess no
     *  faster than a 128-bit key, short enough to be a header value. */
    private static final Pattern USABLE = Pattern.compile("[A-Za-z0-9._~+/=-]{22,256}");

    private static final SecureRandom RANDOM = new SecureRandom();

    private final Path   file;
    private final byte[] value;

    private ApiToken(Path file, String text) {
        this.file  = file;
        this.value = text.getBytes(StandardCharsets.US_ASCII);
    }

    /** Where the token is stored. */
    public Path file() { return file; }

    /** True if {@code presented} is the token. Takes the same time whatever part of it is right. */
    public boolean matches(String presented) {
        return presented != null && MessageDigest.isEqual(presented.getBytes(StandardCharsets.UTF_8), value);
    }

    /** The token in {@code file}, or a new one if there is none (or none that is still secret and usable). */
    public static ApiToken loadOrCreate(Path file) throws IOException {
        file = file.toAbsolutePath();
        UserPrincipal me = OwnerOnly.self();

        String text = readUsable(file, me);
        if (text == null) {
            text = newText();
            publish(file, text);
            log.info("created the API token file {}", file);
        }
        return new ApiToken(file, text);
    }

    static String newText() {
        byte[] raw = new byte[BYTES];
        RANDOM.nextBytes(raw);
        return Base64.getUrlEncoder().withoutPadding().encodeToString(raw);
    }

    /** The text in {@code file} if it can still be trusted, otherwise null (absent, or replaced on purpose). */
    private static String readUsable(Path file, UserPrincipal me) throws IOException {
        PosixFileAttributes a;
        try {
            a = Files.readAttributes(file, PosixFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        } catch (NoSuchFileException absent) {
            return null;
        } catch (UnsupportedOperationException e) {
            throw new IOException("this file system has no POSIX permissions, so " + file
                                  + " cannot be kept private; set fluxdm.api.token-file to a path on one that does", e);
        }
        if (!a.isRegularFile()) {
            throw new IOException(file + " is not a regular file (a symbolic link or a directory?); remove it, "
                                  + "or set fluxdm.api.token-file");
        }
        if (!me.equals(a.owner())) {
            throw new IOException(file + " belongs to another user; remove it, or set fluxdm.api.token-file");
        }
        if (OwnerOnly.opensToOthers(a.permissions())) {
            log.warn("{} was readable by other users ({}), so its token is no longer secret; replacing it",
                     file, PosixFilePermissions.toString(a.permissions()));
            return null;
        }

        byte[] raw;
        try (InputStream in = Files.newInputStream(file, LinkOption.NOFOLLOW_LINKS)) {
            raw = in.readNBytes(1024);                          // a token is far shorter: anything longer is not one
        }
        String text = new String(raw, StandardCharsets.US_ASCII).strip();
        if (!USABLE.matcher(text).matches()) {
            log.warn("{} does not hold a usable token (it must be 22 to 256 characters of A-Z a-z 0-9 . _ ~ + / = -); "
                   + "replacing it", file);
            return null;
        }
        return text;
    }

    /** Write {@code text} to {@code file} so that it appears complete, and with mode 0600 from the first byte. */
    private static void publish(Path file, String text) throws IOException {
        Path dir = file.getParent();
        Files.createDirectories(dir, PosixFilePermissions.asFileAttribute(OwnerOnly.DIR));
        Path tmp = Files.createTempFile(dir, ".api-token-", ".tmp", PosixFilePermissions.asFileAttribute(OwnerOnly.FILE));
        try {
            Files.write(tmp, text.getBytes(StandardCharsets.US_ASCII));     // no trailing newline: nothing to trim
            Files.move(tmp, file, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException | RuntimeException e) {
            try { Files.deleteIfExists(tmp); } catch (IOException suppressed) { e.addSuppressed(suppressed); }
            throw e;
        }
    }
}
