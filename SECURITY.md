# Security policy

FluxDM downloads files from the network and writes them to disk on behalf of a local API, so security
matters here. This page says how to report a problem, what is protected today, and, just as
important, what is not.

## Supported versions

FluxDM is pre-1.0. Only the latest release and the `main` branch receive security fixes.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Use GitHub's private vulnerability reporting: open the repository's **Security** tab and choose
**Report a vulnerability**
([direct link](https://github.com/PhillipWanjiru/Fluxdm/security/advisories/new)).

Please include:

- the affected component (core, daemon, protocol, ...) and the version or commit;
- what an attacker needs to be able to do (reach the API, control a server you download from, run code
  as another local user, ...);
- steps to reproduce, or a proof of concept;
- the impact as you understand it.

What to expect: an acknowledgement within a week, an assessment, a fix developed privately where the
issue is real, and credit in the advisory and the changelog if you want it. Please allow a reasonable
time to fix the problem before you disclose it.

## Scope

**In scope**

- Memory-safety and logic errors in the C core that a remote server or a local client can trigger:
  parsing of HTTP responses, redirects, chunked encoding, the IPC frame decoder, the `.fluxpart`
  resume file, path handling, TLS verification.
- Request validation and path handling in the daemon's REST and STOMP interface.
- Anything that lets a web page, another local user or another process do something to a user's
  downloads that they did not intend.

**Out of scope**

- The desktop UI, the browser extension and the video grabber, which are prototypes
  ([ROADMAP](docs/ROADMAP.md)).
- Exposing the daemon beyond `127.0.0.1`, which is unsupported (see below).
- Denial of service by someone who is already allowed to use the API.
- Attacks that need root or the same user account on the machine.

## What is protected today

- **TLS verification cannot be switched off.** The core verifies the peer certificate against the
  system trust store and checks the host name, or the IP address for literal addresses. TLS 1.2 is the
  minimum. A failed verification ends the connection before any request is sent.
- **The core's IPC socket is mode `0600` and sits in a directory only you can enter**:
  `$XDG_RUNTIME_DIR/fluxdm/` when that is a private directory of yours, otherwise `~/.fluxdm/run/`,
  created mode `0700`. A symbolic link, a file or another user's directory in its place is refused, so
  nobody else can plant a socket for the daemon to adopt. The core refuses to take over a socket that a
  live process serves and never deletes a path that is not a socket.
- **Network and IPC input is validated.** Header sizes, chunk sizes, `Content-Range` against the range
  that was requested, frame magic, version and length, URL and path lengths, embedded NUL bytes, and
  the structure of a `.fluxpart` file before any of it is used. The decoders are tested with garbage,
  truncated and oversized input under AddressSanitizer and UndefinedBehaviorSanitizer.
- **A resume is refused when the remote file changed** (validator and size mismatch), so a partial
  file is never spliced with a different version of the file.
- **A file that fails checksum verification is deleted**, not left at its final name.
- **Credentials go only where they were meant to.** Basic-authentication credentials in a URL are
  sent to that URL's origin and are not carried over a redirect to another host, another port or a
  downgraded scheme.
- **Secrets stay out of logs.** URL credentials and query strings (where signed URLs keep their
  secrets) are stripped from log lines.
- **The API listens on `127.0.0.1` only** (`server.address`), **and checks every request**, in this order:
  1. `Host` must be `127.0.0.1`, `localhost` or `[::1]` (`fluxdm.api.allowed-hosts`; the port does not
     matter). A web page whose own host name has been pointed at `127.0.0.1` ("DNS rebinding") still sends
     its own name, so it is refused.
  2. `Origin`, when the request has one, must be on the allow-list (`fluxdm.api.allowed-origins`: the
     React dev server and the Electron app). Entries are exact strings; wildcards are not accepted, and the
     origin `null` (sandboxed frames, `data:` pages, `file://`) is refused. This covers cross-site requests,
     CORS preflights and the WebSocket handshake.
  3. The request must carry `Authorization: Bearer <token>`: 256 random bits kept in
     `~/.fluxdm/api-token` (mode `0600`), created on the first start, kept across restarts and compared in
     constant time. The STOMP endpoint takes the token in the `CONNECT` frame, because a browser cannot add
     a header to a WebSocket handshake, and refuses every other frame until a `CONNECT` has succeeded. If
     the daemon cannot prepare the token file safely, it does not start.

  A refusal is a fixed `401` or `403` text that never repeats what the client sent, and is logged at most
  once every 30 seconds per kind, with control characters removed from what the client sent.
- **The daemon's data directory `~/.fluxdm` is mode `0700`.** The download database in it lists every URL
  and destination path. A directory that an earlier version left open is closed at start.

## Known limitations

These are real, they are tracked on the [roadmap](docs/ROADMAP.md), and they are the reason the API
must stay on loopback.

1. **The token keeps out web pages, other accounts, and programs that cannot read your home directory.
   It does not keep out other programs running as you.** Anything that runs as your user can read
   `~/.fluxdm/api-token`, as it can read your SSH keys, and can write files wherever you can without
   asking the daemon. There is one token for all clients and it does not expire. To rotate it, stop the
   daemon, delete the file and start the daemon again; clients read the new one.
2. **A socket path you set yourself is used as given.** `fluxdm.core.socket-path` and `fluxdm-core --sock`
   skip the private-directory rules above. Keep such a socket in a directory only you can enter.
3. **The daemon writes wherever its user can write.** That is the job of a download manager, and it is
   why the token matters. There is no sandbox and no allow-list of destination directories.
4. **A file that changes in place without changing size during a single uninterrupted run is not
   detected** (only a resume checks the validators).
5. **Redirects may cross hosts and schemes**, including from `https` to `http`, as in most download
   tools. They are followed without credentials (see above), but the content that arrives is whatever
   the final server sends: verify checksums for anything important.

## Hardening checklist for users

- Keep `server.address=127.0.0.1`. Do not publish the port, forward it, or put it behind a proxy, and do
  not add other names to `fluxdm.api.allowed-hosts`: exposing the API beyond loopback is unsupported.
- Leave `fluxdm.api.auth.enabled` on (`false` is a development switch) and keep the token private: do not
  commit it, paste it into a script you share, or put it in a URL. If it leaks, rotate it (limitation 1).
- Do not run the daemon or the core as root.
- If you set `fluxdm.core.socket-path`, keep the socket in a directory only you can enter (limitation 2).
- Verify checksums for anything important (`checksumType` / `checksum` in the API).
