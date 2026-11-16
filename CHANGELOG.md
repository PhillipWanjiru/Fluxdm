# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before 1.0, a minor version may
contain incompatible changes; they are listed under **Changed** or **Removed** and marked
**BREAKING**.

## [Unreleased]

### Added

- **A design for capturing video in the browser** ([docs/VIDEO_CAPTURE.md](docs/VIDEO_CAPTURE.md)): how an
  extension recognises the media a page plays, how requests are grouped into streams, how HLS and DASH
  manifests are read, and how many connections to open and in what order, each as a formula with a worked
  example. Nothing in the product does this yet. `tests/capture_model_check.py` runs 20 checks of the formulas
  against brute force, exhaustive search and simulation; `tests/mutation_capture.py` breaks 23 of them to show
  that the checks notice; CI runs both.
- Settings `fluxdm.api.allowed-hosts`, `fluxdm.api.allowed-origins`, `fluxdm.api.auth.enabled` (a
  development switch that turns off the token check only; `Host` and `Origin` stay enforced) and
  `fluxdm.api.token-file`.
- The UI prototype sends the token: the Electron app reads it from `~/.fluxdm/api-token`, and `npm start`
  in a browser takes it from `REACT_APP_FLUXDM_TOKEN`.
- `tests/smoke_api.sh`, which starts the built daemon and checks the defences below over real HTTP and
  WebSocket (CI runs it after the Maven build), and the `ApiSecurityTest` suite for the logic behind them.
- **The desktop UI is connected to the daemon.** The downloads list follows the daemon live (STOMP over SockJS:
  a snapshot, then events, taken again after any gap) instead of polling, and shows the daemon's own progress,
  speed and time left. A download can be added (with a destination), paused, resumed, retried and removed, and the
  daemon's refusals appear in plain words. There are views for the Scheduler (resume a paused or failed download
  at a time, once or repeating) and Settings (the daemon's global speed limit). The sidebar shows the real total
  speed and the daemon's health, and a banner says when the daemon or its core is gone.
- `npm run lint` (ESLint, no warnings allowed; CI runs it) and `npm run test:electron` (the Electron shell's rules
  against a stand-in for Electron). 405 Jest tests and 50 shell tests; the CI `ui` job runs both, with the
  production build. `tests/mutation_electron.py` breaks the shell's rules one at a time and checks that a test
  notices (72 changes; not part of CI).
- **An audit of every phase** ([docs/AUDIT.md](docs/AUDIT.md)): for each phase, what the plan asked for, what
  exists, how it was checked, what was fixed and what is left; the plan for the hardening, refactoring and
  optimisation pass that follows the last phase; seven decisions for the maintainer; and a register of 48 open
  items, each with a class and a size. The defects the audit fixed are listed under **Fixed**.

### Changed

- **BREAKING:** requests to the REST API need the token and get `401` without it. STOMP clients send it as
  the `Authorization` header of the `CONNECT` frame. For `curl`:
  `-H "Authorization: Bearer $(cat ~/.fluxdm/api-token)"`. Any other client of the API, such as the browser
  extension skeleton, stops working until it sends the token.
- **BREAKING:** a request whose `Host` is not `127.0.0.1`, `localhost` or `[::1]`, or whose `Origin` is not on
  the allow-list, gets `403` even with the token. The WebSocket endpoint used to accept any origin, and CORS
  preflights now allow only the `Authorization`, `Content-Type` and `Accept` headers.
- **BREAKING:** the IPC socket moved (see Security). `fluxdm.core.socket-path` is unset by default and, when
  set, is used as given; `fluxdm-core --sock` defaults the same way. The core and the daemon must agree, so
  upgrade them together, and stop a core that is still running from an older daemon (`pkill -x fluxdm-core`):
  the new daemon does not look under `/tmp`.
- **BREAKING:** the packaged desktop app serves its page as `app://-/` from the Electron main process instead of
  opening `build/index.html` from `file://`, whose requests carry `Origin: null` and are refused by the daemon.
  Run `npm run build` in `ui/` before `npm run electron`; without a build the app now says so.
- **BREAKING:** the preload script exposes `window.fluxdm.apiToken()` only. The generic `send` and `receive`
  channels are gone; nothing used them.
- `npm run electron:dev` sets `NODE_ENV=development` for Electron. It used to load the production build while it
  waited for the dev server; now it shows the dev server, and ends it when the window closes.

### Removed

- **BREAKING:** the Video Grabber view of the desktop UI. It asked the daemon to look a page up with yt-dlp.
  FluxDM is moving to what the browser itself plays, which needs no extractor; the design is in
  [docs/VIDEO_CAPTURE.md](docs/VIDEO_CAPTURE.md). Until the capture exists, paste the address of the media
  file into **+ ADD URL**.
- **BREAKING:** `POST /api/grabber/detect` and `POST /api/grabber/grab`, and the Python service they
  forwarded to (`grabber/`: FastAPI and yt-dlp), together with the daemon's `GrabberController`,
  `GrabberService` and `PythonBridge`. The endpoints are gone (`404`). The service was a prototype that the
  daemon never started, and the log section `[grabber]` of `config/logging.conf` went with it.

### Fixed

- **A download from a server that cannot resume no longer retries for ever.** Such a server can only stream from
  the first byte, so when it breaks the stream the core starts again from zero. It counted every attempt that got
  16 KiB in as progress and refilled the segment's eight retries each time, so a server that always cut the stream
  at the same place kept the download reconnecting every half second, fetching the same bytes again, and the
  download never failed. Progress now has to get past the furthest point any attempt reached; a download that
  does not get further fails after eight retries, like any other. A server that can resume is not affected: each
  retry carries on after the bytes already written. Found by fuzzing with hostile servers. Against a local
  server that cuts the stream at 200,000 bytes, the old core made 220 requests in 100 s and was still going; the
  new one gives up after 9 requests, in about a minute. The behaviour suite gained 6 checks, 3 of which fail on
  the old core.
- **The C++ engine's parsers no longer trust the files they read** (the engine is not linked into the product
  yet, so nothing shipped was affected). Fuzzing and Valgrind found: a `.torrent` nested 100,000 levels deep
  overflowed the stack, as did a `Content-Disposition` file name of 30 KB or more (`std::regex` recursion; the
  core refuses a response head above 16 KB, so it never got that far); a string length near 2^64 wrapped past
  the bounds check, and numbers such as `i+5e` and `i5xe` were read as 5; a file path such as `../x` inside a
  torrent was accepted; the SHA-1 info-hash was taken from a re-encoded copy of the dictionary instead of the
  file's own bytes, so it was wrong whenever the keys were not already sorted; the last piece was always given
  the full piece length; some fields were never initialised; and the base32 decoder shifted a signed value.
  The parser now limits nesting, checks every length before it uses it, accepts only plain names as path
  components, and hashes the bytes it was given. `tests/test_engine.cpp` gained 63 checks; against the old
  code they crash the process, and 31 of the others fail.
- **`TorrentEngine::next_piece` no longer offers pieces a peer has not claimed.** A peer's bitfield that stopped
  short, or an empty one, was read as "has the rest": an empty bitfield answered piece 0, and one with three
  entries answered piece 3 once pieces 0 to 2 were done. It now offers nothing past the bitfield's end. 21 new
  checks (149 to 170); 4 of them fail on the old code. (The engine is not linked into the product yet.)
- **The browser extension no longer loses downloads.** It cancelled every download the browser started and handed it
  to nothing: its native-messaging host answered `ok` to every message, and nobody was listening to the extension's
  own announcement. It now offers each download to the host and cancels the browser's copy only when the host
  answers `accepted`; no host, a crashing, silent or failing host, or any other reply leaves the download with the
  browser. Downloads from private windows are never offered. The host does not answer `accepted` until it can
  pass a download on (not built yet), validates what it is sent, answers bad JSON and messages that are not objects
  instead of exiting (it quit on `{}`, `[]` and `0`), refuses frames over 64 KB instead of reading the length it was
  told, and is executable. The manifest asks only for `downloads` and `nativeMessaging`: `declarativeNetRequest`,
  `storage` and access to every site were unused, and an empty script was injected into every page. The unused
  `native_bridge.js` is gone. New: `tests/run_extension_tests.sh` (13 Node tests with a stand-in for the browser's
  API, 17 Python tests over real pipes) and an `extension` job in CI. The old script fails all 5 tests that can
  run against it, and the old host 13 of the 17.

### Security

- **The local API checks where every request comes from.** `Host` must be a loopback name, which stops DNS
  rebinding. `Origin`, when a request has one, must be on an allow-list (the React dev server and the Electron
  app by default; wildcards and `null` are refused), which stops cross-site requests and WebSocket hijacking by
  web pages. Every REST call, and the STOMP `CONNECT` frame, must carry `Authorization: Bearer <token>`: a
  random 256-bit secret in `~/.fluxdm/api-token` (mode `0600`), created on the first start. It keeps out web
  pages and other accounts, not other programs running as you. See [SECURITY.md](SECURITY.md).
- **The IPC socket no longer lives in `/tmp`.** It is `core.sock` in `$XDG_RUNTIME_DIR/fluxdm/` or, if that is
  not a private directory of yours, in `~/.fluxdm/run/`: a directory created mode `0700` and refused if it is
  a link, a file or somebody else's. Another local user can no longer plant a socket for the daemon to adopt.
- **`~/.fluxdm` is mode `0700`.** It is created that way, and an existing one that was open is closed at start.
  The database in it lists every URL and destination path.
- **The desktop app's page is confined.** It runs sandboxed without Node, under a Content-Security-Policy that lets
  it load code only from the app and talk only to the daemon (`127.0.0.1:7171`, HTTP and WebSocket); it cannot
  navigate away, open windows or be granted a permission; and only its own top-level page can ask the main process
  for the API token. See [SECURITY.md](SECURITY.md).
- **The resume file `<download>.fluxpart` is private and holds no password.** It was created with the default
  permissions (`0644` on most systems), and written through whatever its temporary name pointed to: a link
  planted at `<download>.fluxpart.tmp` in a shared folder made the core overwrite the file the link pointed to, and
  a FIFO in place of the file made the download wait for ever. It also kept the URL as given, password included.
  It is now mode `0600`, never opened through a link, refused if it is a FIFO or a folder, and the URL is stored
  without its `user:password@` part (a file from an older version, which has it, still matches). The unit suite
  grew from 272 to 343 checks, 25 of which fail on the old code and one of which hangs it; the behaviour suite
  checks the mode and the password across a real restart.
- **Error messages about a URL no longer repeat its password.** When the core refused a URL (no host, a bad port, an
  unsupported scheme, a redirect to one), its error named the URL in full, `user:password@` included, and the
  daemon logged that at WARN and kept it in the download's error message. Confirmed against the real core with
  `http://alice:hunter2@/x`. The messages now name the URL without its user information, through one helper,
  `http_url_redact`, which the resume file shares (it moved here from `resume.c`). The unit suite grew from 343
  to 377 checks, and the behaviour suite has eight new ones, four of which fail on the old core.
- **The `Host` check compares exactly what was sent.** Unicode spaces around a `Host` value were removed, and the
  Kelvin sign (U+212A) was read as a `k`, before the value was looked up. A real request is unlikely to carry
  such characters, and no way to use this was found, but only printable ASCII with spaces or tabs around it is
  accepted now.

## [0.1.0] - 2026-10-28

First public release. The C core and the Java daemon work end to end and are heavily tested; the
desktop UI, the browser extension and the video grabber are prototypes (see
[docs/ROADMAP.md](docs/ROADMAP.md)).

### Added

**Core (`core/`, C11)**
- Multi-segment HTTP and HTTPS downloads driven by one `epoll` loop per download: up to 64 segments,
  4 KiB-aligned boundaries, `fallocate(2)` pre-allocation, writes coalesced into 256 KiB `pwrite()`s.
- A ranged-`GET` probe that discovers size, range support and validators, follows up to 10 redirects,
  and hands its connection to a single stream when the server ignores `Range`.
- Chunked transfer-encoding, `Content-Length` and read-until-close bodies, Basic authentication from URL
  userinfo, `Accept-Encoding: identity`.
- Crash-safe resume: per-segment checkpoints every 3 s in `<file>.fluxpart`, written atomically, and a
  resume that is refused when the remote file changed (validator and size).
- Streaming MD5 and SHA-256 verification; a file that fails verification is deleted.
- Token-bucket bandwidth limiting, per download and global, adjustable at runtime.
- TLS 1.2 and later through OpenSSL with certificate and host-name (or IP) verification.
- A per-host connection gate that learns what a server tolerates from `HTTP 429` / `503`, honours
  `Retry-After`, queues segments instead of hammering, and measures patience against the host's
  silence rather than the queue. Configurable with `--patience`.
  See [docs/RATE_LIMITING.md](docs/RATE_LIMITING.md).
- Retries with jittered exponential back-off; permanent failures reported with stable error codes.
- A `User-Agent` that identifies the project (`FluxDM/0.1.0 (+https://github.com/PhillipWanjiru/Fluxdm)`)
  and `fluxdm-core --version`.

**IPC protocol v1 (`ipc/`)**
- A fixed-layout, little-endian binary protocol over a Unix socket (mode `0600`) with a 16-byte header,
  commands `START`, `PAUSE`, `RESUME`, `CANCEL`, `SET_LIMIT`, `STATUS`, `STATUS_ALL`, `SHUTDOWN`, and
  replies and events distinguished by a flag. Sizes are pinned by `_Static_assert`.
- Back-pressure: progress frames are dropped for slow clients and clients that fall hopelessly behind
  are disconnected; the core never blocks on a client.
  See [docs/IPC_PROTOCOL.md](docs/IPC_PROTOCOL.md).

**Daemon (`daemon/`, Java 21, Spring Boot 3.2)**
- REST API (`/api/downloads`, `/api/bandwidth`, `/api/stats`, `/api/health`, `/api/scheduler`,
  `/api/grabber`) and STOMP-over-WebSocket push (`/ws`, `/topic/downloads`).
- SQLite persistence, a bounded priority queue (`fluxdm.max-concurrent-downloads`), a scheduler for
  one-shot and repeating resumes.
- Core supervision: starts or adopts the core, restarts it after a crash with back-off, and reconciles
  its database with what the core is running, so interrupted downloads continue from their checkpoint.

**Engine layer (`engine/`, C++17)**
- Building blocks, built and tested standalone: segment manager, adaptive tuner, buffer manager,
  bencode and `.torrent` metadata parsing, magnet links, SHA-1, peer discovery.

**Prototypes**
- Desktop UI (React, Redux, Electron), browser extension (Manifest V3) and video grabber (FastAPI,
  yt-dlp).

**Tests and tooling**
- C unit suites under AddressSanitizer and UndefinedBehaviorSanitizer, an integration suite, a
  behaviour suite that drives the real core through scriptable mock servers, ThreadSanitizer builds,
  and Java suites that run the daemon against a scripted fake core and the real binary.
  See [docs/TESTING.md](docs/TESTING.md).
- Continuous integration (hygiene checks, build and test of the core, sanitizer builds, the daemon with a
  real Spring build, the engine layer), a release workflow that publishes a tag's changelog entry, local git
  hooks that enforce Conventional Commits and keep secrets and large files out, issue forms, a pull-request
  template, a code of conduct, and documentation ([docs/](docs)).

### Security
- TLS verification cannot be disabled; credentials in URLs are never carried to another origin; URL
  credentials and query strings are stripped from logs.
- The API is unauthenticated and does not yet validate `Host` or `Origin`; it must stay on
  `127.0.0.1`. See [SECURITY.md](SECURITY.md#known-limitations).

[Unreleased]: https://github.com/PhillipWanjiru/Fluxdm/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/PhillipWanjiru/Fluxdm/releases/tag/v0.1.0
