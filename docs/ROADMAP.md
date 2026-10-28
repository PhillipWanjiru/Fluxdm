# Roadmap

Where FluxDM is, what comes next, and what it cannot do yet. This is a plan, not a promise: order
follows dependencies and what hurts most, and it changes when something is learned.

- [Where things stand](#where-things-stand)
- [Next](#next)
- [Later](#later)
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

**Prototypes, not wired in yet:** the React / Electron UI, the Manifest V3 extension, the Python
video grabber, and the C++ engine layer.

## Next

In rough order. Each item says what it depends on, so the order is not arbitrary.

1. **Harden the local API.** The daemon has no authentication and does not validate the `Host` or
   `Origin` of requests, which matters for a service that writes files where it is told to
   ([SECURITY.md](../SECURITY.md#known-limitations)). Validate `Host` against loopback names (this
   defeats DNS rebinding), restrict the STOMP endpoint's origins, require a custom header or token on
   state-changing requests (this defeats cross-site `POST`), and default the IPC socket to
   `$XDG_RUNTIME_DIR` instead of `/tmp`. This comes first: every later item widens what the API can do.
2. **A "waiting for server" state.** A download queued behind a host's connection limit currently
   reports as running with no progress. The core knows exactly why it is waiting; it should say so.
   Needs one additional status value in the protocol (an additive change that older readers can
   ignore), a daemon status and a UI badge.
3. **Connect the UI to the daemon.** The components and styles exist, and the downloads list polls
   `GET /api/downloads`, but the integration is unfinished. Concretely: give `react-scripts` the entry
   point it expects (`src/index.js`; the file is `main.jsx` today); map the daemon's integer statuses
   (0 to 4) to the states the UI filters on (`downloading`, `queued`, `complete`); replace the live-speed
   graph, which is placeholder data, with real speeds from `/api/stats` or the STOMP feed; mount the
   scheduler, settings and grabber views (the shell renders only the downloads list); use STOMP over
   SockJS (`/ws`, `/topic/downloads`) instead of polling, since the raw `useWebSocket` hook cannot speak
   it; and fix the `getStatus` call, which targets an endpoint that does not exist
   (`/downloads/{id}`, not `/downloads/{id}/status`). Depends on nothing else, and it makes the project
   usable by someone who does not use `curl`.
4. **Per-segment progress.** The protocol reports aggregate progress only, so the UI's segment bars
   have nothing to draw. Add a per-segment breakdown to `STATUS` (a protocol change, so a version
   bump or an optional trailing section) and persist nothing: the core already tracks it.
5. **Request headers in `START`.** Cookies, `Referer` and `User-Agent` are what make a browser
   hand-off work for downloads behind a login. Today the protocol has no place for them. Needs a
   protocol extension and daemon support first, then:
6. **Browser-extension hand-off.** Today the extension cancels the browser's download and nothing
   receives the URL: the native-messaging host only acknowledges messages. Pass URL and headers to the
   daemon through the host, and show progress in the popup.
7. **Grabber service.** Start and supervise the Python service from the daemon, align the two sides
   (the daemon's bridge posts a JSON body; the grabber reads query parameters), and let a page URL
   turn into a format choice and then into an ordinary download.
8. **Validate on every segment.** Send `If-Range` with the probe's validator and compare `ETag` /
   `Last-Modified` on each response, so a file that changes in place *without changing size* during a
   single run is detected. Today only a size change is caught outside a resume.

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
| Progress | Aggregate only; no per-segment breakdown over IPC. |
| API | REST and STOMP have no authentication and do not validate `Host` or `Origin`; keep them on `127.0.0.1` ([SECURITY.md](../SECURITY.md#known-limitations)). |
| Engine layer | Not linked into the core. |
| UI, extension, grabber | Prototypes. |
