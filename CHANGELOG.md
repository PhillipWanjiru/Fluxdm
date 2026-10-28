# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before 1.0, a minor version may
contain incompatible changes; they are listed under **Changed** or **Removed** and marked
**BREAKING**.

## [Unreleased]

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
