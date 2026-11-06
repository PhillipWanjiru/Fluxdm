# Roadmap

Where FluxDM is, what comes next, and what it cannot do yet. This is a plan, not a promise: order
follows dependencies and what hurts most, and it changes when something is learned.

- [Where things stand](#where-things-stand)
- [Next](#next)
- [Later](#later)
- [Before 1.0: the audit](#before-10-the-audit)
- [Known limitations](#known-limitations)

## Where things stand

**Released as 0.1.0**

- [x] C core: epoll-driven multi-segment HTTP / HTTPS engine, crash-safe resume, MD5 / SHA-256
      verification, token-bucket limiter, TLS with certificate and hostname / IP verification
- [x] Versioned binary IPC protocol with back-pressure ([IPC_PROTOCOL.md](IPC_PROTOCOL.md))
- [x] Per-host connection gate, `Retry-After`, silence-based patience ([RATE_LIMITING.md](RATE_LIMITING.md))
- [x] Java daemon: REST, STOMP push, SQLite persistence, bounded priority queue, scheduler, core
      supervision and recovery after a crash or restart of either process
- [x] Unit, integration, behaviour and daemon test suites; ASan, UBSan and TSan builds
      ([TESTING.md](TESTING.md))

**Unreleased** ([CHANGELOG.md](../CHANGELOG.md))

- [x] Local API hardening: `Host` and `Origin` allow-lists, a bearer token on REST and STOMP, and a
      private directory for the IPC socket and for `~/.fluxdm` ([SECURITY.md](../SECURITY.md))
- [x] Desktop UI connected to the daemon: the downloads list follows it live, with add, pause, resume,
      retry and remove, real speeds, a Scheduler, a Video Grabber lookup, the global speed limit and the
      daemon's health; the Electron shell serves the page as `app://-/` under a Content-Security-Policy
      ([ARCHITECTURE.md](ARCHITECTURE.md#4-the-desktop-ui))

**Prototypes, not wired in yet:** the Manifest V3 extension, the Python video grabber, and the C++
engine layer.

## Next

In rough order. Each item says what it depends on, so the order is not arbitrary.

1. **A "waiting for server" state.** A download queued behind a host's connection limit currently
   reports as running with no progress. The core knows exactly why it is waiting; it should say so.
   Needs one additional status value in the protocol (an additive change that older readers can
   ignore), a daemon status and a UI badge.
2. **Per-segment progress.** The protocol reports aggregate progress only, so the UI draws the overall
   progress in segment-sized steps, not each segment's own. Add a per-segment breakdown to `STATUS` (a
   protocol change, so a version bump or an optional trailing section) and persist nothing: the core
   already tracks it.
3. **Request headers in `START`.** Cookies, `Referer` and `User-Agent` are what make a browser
   hand-off work for downloads behind a login. Today the protocol has no place for them. Needs a
   protocol extension and daemon support first, then:
4. **Browser-extension hand-off.** Today the extension cancels the browser's download and nothing
   receives the URL: the native-messaging host only acknowledges messages. Pass URL and headers to the
   daemon through the host (with the API token, which the host can read from `~/.fluxdm/api-token`), and
   show progress in the popup. The same hand-off can carry **the media a page plays**: tools in IDM's
   family are understood to watch the page's own media requests (an extension can see them with
   `webRequest`) and pass the URL and headers on, which is why they do not depend on a site-specific
   extractor. Vendors do not document this, so treat it as inference. FluxDM then has two routes to a
   video: this capture, and yt-dlp through the grabber (current yt-dlp releases may need an external
   JavaScript runtime for YouTube; check that the grabber's environment has one).
5. **Grabber service.** Start and supervise the Python service from the daemon (it is expected at a
   hard-coded `http://127.0.0.1:17172` today), and let a page URL turn into a format choice and then into
   an ordinary download. Known problems to fix on the way: the daemon forwards `url` and `format_id`
   without URL-encoding, so an address containing `&`, `#` or `+` is cut or altered; it reports a failed
   call as HTTP 200 with an `error` field; and the service's `/grab` downloads the whole file inside its
   event loop before it answers, into its own folder, so the file bypasses FluxDM's downloader and is
   not listed.
6. **Validate on every segment.** Send `If-Range` with the probe's validator and compare `ETag` /
   `Last-Modified` on each response, so a file that changes in place *without changing size* during a
   single run is detected. Today only a size change is caught outside a resume.
7. **Measure FluxDM against curl, wget and aria2, and use them where they win.** The question is not which
   tool is best but when each one finishes a given download first, and the answer has to come from a model
   and measurements. The model: a download of `S` bytes takes `T = T_setup + S / R(n) + T_tail`, where
   `T_setup` is everything before the first payload byte (DNS, TCP, TLS, the probe, the extra connections),
   `R(n) = min(B, n·r_c, r_server)` is the rate with `n` connections (`B` the link, `r_c` one connection's
   rate, `r_server` any cap the server applies) and `T_tail` is the wait for the slowest segment. Comparing
   `n` connections with one gives a break-even size `S* ≈ ΔT_setup · r_c · n / (n − 1)`, where `ΔT_setup` is
   the extra start-up cost of the `n` connections: below `S*` a single connection finishes first, above it the
   segments do. So a tool with a cheaper start should win small files and a tool with a better `R(n)` large
   ones. Where a tool beats FluxDM by more than the noise, FluxDM changes: either the gap is closed in its own
   engine (the "Later" items below are the candidates) or the transfer is handed to that tool in the cases
   where it wins. The plan:
   - A benchmark in `bench/` that builds a network namespace with `tc netem` (bandwidth, latency, loss, a
     per-connection cap, a server that limits connections) and serves files of different sizes from it, so the
     conditions are reproducible and not the internet's mood.
   - At least 20 interleaved runs per tool and condition (A, B, C, A, B, C, … so that drift touches every tool
     alike), medians with bootstrap confidence intervals, and a Mann–Whitney test with Cliff's δ as the effect
     size: a difference counts when its interval excludes zero *and* the effect is not small.
   - Delegation, if the numbers call for it: aria2 through its JSON-RPC interface as an optional backend the
     daemon can pick per download, and curl through `libcurl` where its connection reuse, HTTP/2 or proxy
     support beat the core's. The daemon chooses from the model and the measured `S*`, not from a hard-coded
     rule.

## Later

- **Dynamic segment rebalancing.** When a segment finishes early, split the largest remaining range
  instead of leaving a connection idle. This is what the engine layer's `SegmentManager` and
  `AdaptiveTuner` are for; integrating them is the biggest remaining throughput win on uneven
  connections.
- **Connection reuse and TLS session resumption.** Requests currently use `Connection: close`, so every
  retry pays a new TCP and TLS handshake. The connection pool code exists; it is not used.
- **Proxy support.** `http_proxy` / `https_proxy` environment variables and `CONNECT`.
- **Packaging.** A `systemd --user` unit, a `.deb`, and release artifacts built by CI.
- **Persistent host knowledge.** Remember what each host tolerates across restarts, so the first
  download after a restart does not have to rediscover a connection limit.
- **BitTorrent.** The engine layer has the building blocks (bencode, metadata, magnet links, peer
  discovery, SHA-1); a working client is a separate, large piece of work.

## Before 1.0: the audit

Before a 1.0 release the whole project gets one pass, from the first phase (the C core) outward through the
engine layer, the daemon, the grabber and the extension, and the UI. The goal is industry-grade code that
installs and runs reliably on a machine that is not the author's:

- **Refactor and optimise** what the phases left rough; **remove the stubs** and everything nothing uses.
- **Make it installable elsewhere.** No path tailored to one desktop (the daemon's default core path is
  `~/Desktop/fluxdm/core/fluxdm-core`; the grabber's address is fixed in the daemon), a `systemd --user` unit
  and a `.deb`, a documented install and upgrade, and a clean-machine test in CI.
- **Hold every part to the same standard**: lint, tests and sanitizers where they apply, documented behaviour,
  no warnings.

Candidates found so far: a list to start from, not the audit.

*UI and shell*

- Unused code: `components/Sidebar/CategoryList.jsx`, `electron/tray.js`, `getStats` in `api/stats.js`.
- One shared modal with a focus trap and focus return; the three dialogs repeat their overlay code and none
  returns focus to what opened it.
- Electron 29 is no longer supported upstream: move to a supported major, then re-run the shell's tests and a
  real-Electron smoke test. `react-scripts` is unmaintained (its build tooling carries 15 high-severity advisories,
  none of them in the shipped app: `npm audit --omit=dev` reports none): move to Vite. recharts 2 is deprecated:
  move to 3.
- Dim text (`--text-dim: #666` on `#0a0a0a`) is about 3.5 : 1, under the 4.5 : 1 that WCAG AA asks of small text.
- When the daemon is lost the list keeps showing the last state it reported (running, with speeds) and only the
  banner says the connection is gone; consider marking the rows stale.
- Scheduling needs a paused or failed download; offer "add and start later" as one step. Repeating schedules use
  a fixed number of seconds, so across a daylight-saving change they drift by an hour.
- A folder picker for the destination (a narrow IPC next to `apiToken`, like it) instead of a typed path.
- An end-to-end job: start the daemon the `daemon` job already builds and drive the built page in a browser, in
  place of the stand-ins the UI is tested against today.
- The packaged app needs `chrome-sandbox` set up by hand on Ubuntu 24.04 and later ([README](../README.md#run-the-desktop-ui));
  a `.deb` should do it.

*Daemon*

- `GrabberService` does not URL-encode what it forwards, and `PythonBridge` returns errors as HTTP 200 (see
  "Grabber service" above).
- The STOMP broker sends no heartbeats, so a half-open connection is noticed only by the client's own timeout.
- Check against the running daemon what it answers to SockJS's credentialed `GET /ws/info` from `app://-` and
  from `http://localhost:3000` (the UI's tests and the browser check used stand-ins).

## Known limitations

Stated plainly, so nobody has to find them the hard way.

| Area | Limitation |
|---|---|
| Platform | Linux only. The core uses `epoll`, `eventfd` and `fallocate`. |
| Protocols | HTTP/1.1 and HTTPS only: no FTP, SFTP, HTTP/2 or HTTP/3. |
| Network | No proxy support; the core connects directly. Requests use `Connection: close`. |
| Requests | No cookies, custom headers or `Referer`; Basic authentication from URL userinfo only. |
| Integrity | A file that changes in place without changing size during a single uninterrupted run is not detected (see "Validate on every segment"). A resume does validate. |
| `Retry-After` | Only the delta-seconds form is understood (the HTTP-date form is ignored), capped at one hour. |
| Rate limiting | Host knowledge is per process and in memory, per `host:port`; the table holds 64 hosts. |
| Progress | Aggregate only; no per-segment breakdown over IPC, so the UI's segment bars show the overall progress in steps. |
| API | One token for all clients, readable by every program that runs as you; keep the API on `127.0.0.1` ([SECURITY.md](../SECURITY.md#known-limitations)). |
| Engine layer | Not linked into the core. |
| UI | Downloads go to a folder you type (no folder picker); a video grabbed through the Video Grabber is saved by the grabber in its own folder and does not appear in the list; scheduling needs a paused or failed download; the packaged app runs on Electron 29, which is out of support. |
| Extension, grabber | Prototypes. |
