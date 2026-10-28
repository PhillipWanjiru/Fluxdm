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
- **The core's IPC socket is mode `0600`.** The core refuses to take over a socket that a live process
  serves and never deletes a path that is not a socket.
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
- **The API listens on `127.0.0.1` only** (`server.address`).

## Known limitations

These are real, they are tracked on the [roadmap](docs/ROADMAP.md), and they are the reason the API
must stay on loopback.

1. **The REST and STOMP API has no authentication and does not validate the `Host` or `Origin` of
   requests.** On a single-user machine, anything that can reach `127.0.0.1:7171` can start a download
   to any path the daemon's user can write. Web pages can reach loopback services in some situations:
   through DNS rebinding (a page whose host name is made to resolve to `127.0.0.1`, which turns the
   API into "same origin" for that page), through the STOMP endpoint, which accepts any origin, and
   through simple cross-site `POST` requests such as pause and resume. Browsers are adding protections
   against this class of attack, but do not rely on them. Until host and origin validation and an
   optional token land, treat the daemon as an unauthenticated local service: run it only when you
   need it, and never as root.
2. **The default IPC socket path is under `/tmp`.** The path is per user and the core creates it with
   mode `0600`, but a *different* local user could create a socket at that path first, and the daemon
   would adopt it. On a multi-user machine, point both sides at a private directory, for example
   `fluxdm.core.socket-path=${XDG_RUNTIME_DIR}/fluxdm.sock`.
3. **The daemon writes wherever its user can write.** That is the job of a download manager, but it
   makes limitation 1 more serious. There is no sandbox and no allow-list of destination directories.
4. **A file that changes in place without changing size during a single uninterrupted run is not
   detected** (only a resume checks the validators).
5. **Redirects may cross hosts and schemes**, including from `https` to `http`, as in most download
   tools. They are followed without credentials (see above), but the content that arrives is whatever
   the final server sends: verify checksums for anything important.

## Hardening checklist for users

- Keep `server.address=127.0.0.1`. Do not publish the port, forward it, or put it behind a proxy
  without authentication.
- Do not run the daemon or the core as root.
- On multi-user machines, move the socket into `$XDG_RUNTIME_DIR` (limitation 2).
- Verify checksums for anything important (`checksumType` / `checksum` in the API).
