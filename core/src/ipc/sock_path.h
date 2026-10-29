/* FluxDM — core/src/ipc/sock_path.h
 * Where the core listens when it is not told where, and how that place is kept private.
 *
 * A socket in /tmp can be created first by another user, who then answers whoever connects: the daemon,
 * with the URLs and destination paths it sends. So the default lives in a directory that only this user
 * can enter:
 *
 *     $XDG_RUNTIME_DIR/fluxdm/core.sock    when $XDG_RUNTIME_DIR is an absolute path to a directory that
 *                                          is ours and closed to everyone else (systemd creates it so)
 *     $HOME/.fluxdm/run/core.sock          otherwise
 *
 * The directory is created with mode 0700 and must belong to this user. One of ours that is too open is
 * closed again; a symbolic link, a file, or somebody else's directory is refused.
 *
 * The Java daemon picks the same two places by the same rules (security/RuntimeDir.java) and keeps its
 * API token next to the socket, so a client that knows one knows the other.
 */
#pragma once

#include <stddef.h>

/* Choose the default socket path from the environment ($XDG_RUNTIME_DIR, $HOME, then the password
 * database) and make sure its directory exists and is private. Writes the path to `out`.
 * 0 on success; -1 with the reason in flux_last_error_str(). */
int flux_ipc_default_sock_path(char *out, size_t cap);

/* The same decision from explicit inputs (NULL = not set), so the rules can be tested without touching
 * the process environment. */
int flux_ipc_resolve_sock_path(const char *xdg_runtime_dir, const char *home, char *out, size_t cap);
