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
  at a time, once or repeating), the Video Grabber (look up a page's formats through the daemon, with cancel) and
  Settings (the daemon's global speed limit). The sidebar shows the real total speed and the daemon's health, and
  a banner says when the daemon or its core is gone.
- `npm run lint` (ESLint, no warnings allowed; CI runs it) and `npm run test:electron` (the Electron shell's rules
  against a stand-in for Electron). 459 Jest tests and 50 shell tests; the CI `ui` job runs both, with the
  production build. `tests/mutation_electron.py` breaks the shell's rules one at a time and checks that a test
  notices (72 changes; not part of CI).

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
