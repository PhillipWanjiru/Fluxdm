# Testing

A download manager's bugs live in interleavings: a reset that lands between two `pwrite()`s, a pause
that races a redirect, a server that refuses the fifth connection but not the fourth. So FluxDM is
tested in layers, from pure functions up to the whole system against hostile servers, and the layers
that touch threads and memory run under sanitizers.

- [Quick reference](#quick-reference)
- [The suites](#the-suites)
- [What the behaviour suite covers](#what-the-behaviour-suite-covers)
- [Sanitizer builds](#sanitizer-builds)
- [How the daemon is tested](#how-the-daemon-is-tested)
- [The API smoke test](#the-api-smoke-test)
- [The mock origin server](#the-mock-origin-server)
- [Test quality](#test-quality)
- [Continuous integration](#continuous-integration)
- [Writing tests](#writing-tests)

## Quick reference

```bash
sudo apt install build-essential libssl-dev python3 openjdk-21-jdk     # what the suites need

bash tests/run_all.sh              # C unit + integration + behaviour suites (several minutes)
bash tests/run_all.sh --quick      # the same, without the large throughput run
bash tests/run_all.sh --no-core    # unit tests only (seconds)

bash tests/run_daemon_tests.sh     # Java suites (add --no-e2e to skip the real-core suite, --no-maven to skip the Maven step)
bash tests/smoke_api.sh            # the built daemon's API defences over real HTTP and WebSocket (needs the jar and the core built)
bash tests/run_engine_tests.sh     # C++ engine layer

python3 tests/test_core.py -k rate_limit      # only the behaviour tests whose function name contains "rate_limit"
FLUXDM_CORE=/path/to/fluxdm-core python3 tests/test_core.py    # test another build of the core
```

`tests/run_all.sh` builds the core if `core/fluxdm-core` is missing. Every behaviour and integration
test starts its **own** core on a private socket, so a daemon or core you are running is never touched.

## The suites

| Suite | Command | Needs | What it proves |
|---|---|---|---|
| **C unit** | `make -C tests run-unit` | gcc, `libssl-dev` | Each module in isolation: error store (thread-local), token-bucket limiter (rates, burst depth, debt), streaming MD5 / SHA-256, the `.fluxpart` resume file (round trips, truncation and corruption), the host gate (policy driven by an injected clock), URL parsing and RFC 3986 reference resolution, the HTTP request builder and response-head parser (including `1xx` interim responses, chunked and `Content-Range` handling), and the IPC frame encoder and incremental decoder (split, coalesced, byte-at-a-time, oversized and garbage input). Built with ASan + UBSan. |
| **Integration** | `python3 tests/test_integration.py` | python3 | A smoke test of the real core through the real IPC protocol, against a range-aware local server: the core starts and binds a private (`0600`) socket; a 1 MB download in 4 segments completes with the right SHA-256 and no `.fluxpart` left behind; `STATUS`, `STATUS_ALL`, `SET_LIMIT` and `CANCEL` (idempotent, cleans up) behave; the progress frames of a 10 MB download are flagged as events and never go backwards or past the total; the core exits cleanly on `SHUTDOWN`. |
| **Behaviour** | `python3 tests/test_core.py [--quick] [-k substring]` | python3, `openssl` CLI | The core as a black box against scriptable local HTTP and HTTPS servers. See below. |
| **Daemon** | `bash tests/run_daemon_tests.sh` | JDK 21 (Maven optional) | The Java services against a scripted fake core, and against the real core. See below. |
| **API smoke** | `bash tests/smoke_api.sh` | JDK 21, curl, python3, the built jar and core | The real daemon, started in a throw-away home directory, refuses the wrong Host, the wrong Origin and a missing or wrong token over real HTTP and WebSocket. See below. |
| **Engine** | `bash tests/run_engine_tests.sh` | g++, `libssl-dev` | The C++ engine layer: segment manager, adaptive tuner, buffer manager, bencode and `.torrent` metadata parsing, magnet links, SHA-1, peer discovery. Compiles directly with `g++`; no CMake needed. |

## What the behaviour suite covers

Each area is one or more functions in [`tests/test_core.py`](../tests/test_core.py). Every download it
performs is verified by size and SHA-256 against the origin's file, that no `.fluxpart` is left behind,
and that progress frames never go backwards or past the total.

| Area | Scenarios |
|---|---|
| **Transfers** | File sizes against segment counts; empty files; keep-alive, chunked, unknown-length and no-`Range` servers; URL forms; redirect chains and loops; Basic authentication |
| **Integrity** | Matching and mismatching MD5 / SHA-256 (a mismatch deletes the file and discards the resume state) |
| **Failure paths** | Malformed and unsupported URLs; connection refused; DNS failure; a server that drops every request; 403, 404, 500 with retries; and the original crash: a failing `START` among running downloads |
| **Retries** | Resets mid-body, truncated bodies, back-off and the retry budget |
| **Rate limiting** | `429` / `503` from connection caps and scripted refusals; `Retry-After`; the host gate's learned allowance; the patience clock (silence versus queueing); the `--patience` switch; probes refused repeatedly; log output reduced to one complaint and one settled line; downloads sharing a host; servers that ignore `Range`. See [RATE_LIMITING.md](RATE_LIMITING.md). |
| **Disk errors** | Destination under a regular file or under `/proc`; write failures from a file-size limit; the core survives them |
| **Control** | Pause, resume (also while probing), a storm of pause / resume, cancel cleanup, an interruptible probe, bandwidth limits, duplicate and reserved ids |
| **Crash recovery** | Resume after `SHUTDOWN`, `SIGTERM` and `SIGKILL`; refusal to resume when the remote file changed or the resume file is inconsistent |
| **Concurrency** | Many downloads at once; event broadcast to several clients; a second core refused on a live socket |
| **IPC robustness** | Garbage, oversized frames and bad versions; slow readers (progress dropped, then disconnected); clients vanishing mid-download; the 16-client limit |
| **HTTPS** | A throw-away local CA; verification of DNS and IP subject alternative names; a certificate from an untrusted CA, one for the wrong host name, and one that only names `localhost` when reached by IP are all refused with a TLS error **before any request is sent** |
| **Resources** | No descriptor or thread leak over the whole suite; the core exits cleanly; no sanitizer, assertion or join-check report in its log |
| **Throughput** | A large transfer to catch performance regressions (skipped by `--quick`) |

## Sanitizer builds

The C unit suites are always built with AddressSanitizer and UndefinedBehaviorSanitizer. The **core**
itself can be built both ways and the behaviour suite pointed at it:

```bash
# AddressSanitizer + UBSan
make -C core clean && make -C core debug
cp core/fluxdm-core /tmp/fluxdm-core-asan
FLUXDM_CORE=/tmp/fluxdm-core-asan python3 tests/test_core.py --quick

# ThreadSanitizer (data races)
make -C core clean && make -C core tsan
cp core/fluxdm-core /tmp/fluxdm-core-tsan
FLUXDM_CORE=/tmp/fluxdm-core-tsan python3 tests/test_integration.py

make -C core clean && make -C core          # back to the release build
```

Run `make clean` between flavours: the Makefile reuses object files. The behaviour suite scans the
core's log for sanitizer reports and fails if it finds any. Sanitized binaries are slower, so use
`--quick` or `-k` to select what you run under them.

## How the daemon is tested

`tests/run_daemon_tests.sh` needs only a JDK. It runs five suites:

| Suite | What it covers |
|---|---|
| `ApiSecurityTest` | The local API's defences, driven through stand-in servlet and message objects: the private runtime directory and the core's socket path, the token file (created atomically with mode `0600`; group- or world-readable files, symlinks and files of other users refused), the Host / Origin / token verdicts and their order, the HTTP filter (preflights, what the `/ws` exemption allows and what it must not, fixed error bodies, rate-limited logging), and the token check in the STOMP `CONNECT` frame |
| `QueueServiceTest` | Who may run: slots tracked by id, priority order, idempotent release, adoption |
| `CoreIPCClientTest` | The wire protocol from the client's side: reply versus event matching, ordering, timeouts, reconnects, protocol abuse |
| `DownloadServiceTest` | The download logic against a scripted fake core: validation, queueing, pause / resume / cancel, error handling, recovery after a core or daemon restart, the scheduler, the bandwidth service |
| `DaemonE2ETest` | The same classes against the **real** `fluxdm-core` and a local HTTP server: queueing, pause / resume, cancel, checksums, `kill -9` of the core, a daemon restart that adopts the running core. About 40 s; skip with `--no-e2e`. |

**How the daemon compiles without Spring.** `tests/java/stubs/` holds minimal stand-ins for the few
Spring, Jakarta and SLF4J types the daemon's source mentions (annotations, `SimpMessagingTemplate`,
`JpaRepository`, ...). That type-checks every line of `daemon/src/main` and lets the real service classes
run, with no Maven, no jars and no network.

**What that does not cover.** The stubs are not Spring. Bean wiring, JPA and SQLite, the REST mappings
and the WebSocket endpoint are exercised only by a real build. The script's last step runs
`mvn -q -DskipTests package` when Maven is available and can reach Maven Central, and CI does the same.
The API smoke test below is what exercises Spring's side of the API defences.

## The API smoke test

`ApiSecurityTest` proves the logic of the local API's defences, but it drives the filter and the STOMP
interceptor through stand-ins, so it cannot show that Spring really puts them in front of every request.
[`tests/smoke_api.sh`](../tests/smoke_api.sh) does: it starts the built daemon (`daemon/target/fluxdm-daemon.jar`)
and the real core in a throw-away home directory, on a free port, and checks over real HTTP and a real
WebSocket that

- a request without the token, with a wrong token, or with the token under another scheme gets `401`;
- another `Host` (DNS rebinding) and another `Origin` (including `null`) get `403`, even with the token, and
  the refusal carries no CORS header, so the page that sent the request cannot read it;
- a cross-site `POST` without a preflight starts no download;
- the UI's origin is served with its CORS headers, a preflight needs no token, and a path that only pretends
  to be under `/ws` (`/ws/../api/downloads`) still needs one;
- on the WebSocket, `CONNECT` is refused without the token, and `SUBSCRIBE` and `SEND` are refused until a
  `CONNECT` with the token has succeeded, and accepted after it;
- `~/.fluxdm` and the runtime directory have mode `0700`, the token file `0600`, the core's socket is inside
  the runtime directory, and the token is not in the daemon's log.

```bash
make -C core && mvn -f daemon/pom.xml -DskipTests package      # once
bash tests/smoke_api.sh
bash tests/smoke_api.sh --keep                                 # leave the daemon log and home directory behind
```

Most of the run is the daemon starting. It needs no network and touches nothing outside its own temporary
directory: the daemon runs with `HOME` and `user.home` pointed there, so your real `~/.fluxdm` and token are
not used. A failed run keeps that directory and prints its path.

Like the other suites, the script was checked to be able to fail. It was developed against a stand-in server
with 23 deliberately broken variants (a skipped Host check, a wrong-token check that accepts any bearer, a
token file that is group-readable, a session that forgets it authenticated, ...), and had to fail on the
matching check for every one of them. The stand-in is not part of the repository.

## The mock origin server

[`tests/fluxtest.py`](../tests/fluxtest.py) provides the scriptable HTTP(S) origin (`Site`), a process
manager for the core under test (`Core`: private socket, log capture, descriptor and thread counts) and a
minimal IPC client (`Conn`) that also checks the protocol's own invariants as it goes: replies are not
flagged as events, events are, and no frame is corrupt. A `Site` can:

- serve files with or without `Range`, with keep-alive, chunked or unknown-length bodies, at a chosen
  chunk size and delay per chunk;
- cap concurrent connections (`max_active`) and answer the surplus with `429` or `503` plus
  `Retry-After` (`limit_status`);
- hold every connection open after N bytes until an event is set (`hold_after`, `release`);
- follow a **script** of per-request behaviours (`script=[None, None, (429, None), ...]`), reset
  connections mid-body, redirect, demand Basic credentials, drop or reject `HEAD`, and serve a
  different `ETag` or `Last-Modified`;
- record every request with its timing, so tests can assert on what the server *saw* (peak
  concurrency, refusals, spacing between retries), not only on what the client reported.

TLS tests generate a throw-away CA and certificates with the `openssl` command-line tool.

## Test quality

Passing tests only matter if they can fail. For the rate-limiting code, the host gate in particular,
the tests were validated by **mutation**: dozens of deliberate one-line bugs (ignoring `Retry-After`,
dropping the allowance cut, starting the patience clock at the wrong moment, not counting the adopted
probe connection, leaking a slot on release, comparing host names case-sensitively, ...) were injected
one at a time into a copy of the source, and the relevant suites had to fail for every one. A mutant
that survived was either a missing test (which was then written) or an equivalent change that cannot
alter behaviour (which was recorded as such). The daemon's queue, reconcile and validation logic got the
same treatment. The mutation script was a one-off development tool and is not part of the repository; the
tests it drove are.

Other practices that keep the suites honest:

- **Assertions on what the server saw.** A client that claims "I waited politely" is checked against the
  server's own request log.
- **No sleeps as synchronisation where an event exists.** Tests wait on conditions (a download
  reaching a state, the server's `release` event) with generous upper bounds; lower bounds come from
  server-side delays, so a slow machine makes tests slower, not flaky.
- **Each test starts from a clean slate**: private temp directory, private socket, its own core where
  global state (the host gate, the patience) matters.

## Continuous integration

Every push and pull request runs [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) on a clean
Ubuntu runner:

| Job | Runs |
|---|---|
| **hygiene** | ShellCheck over every shell script; the git-hook tests (`.github/scripts/test-hooks.sh`); the Conventional Commits check over the commits of the push or pull request |
| **core** | Release build of the core; the three C unit suites (ASan + UBSan); the integration suite; the behaviour suite (`--quick`) |
| **core-sanitizers** | The core built with ASan + UBSan, then with ThreadSanitizer, each driven by the integration suite; `UBSAN_OPTIONS` and `TSAN_OPTIONS` are set to `halt_on_error=1`, so a report ends the run instead of scrolling past |
| **daemon** | `mvn package` (compiles the daemon against real Spring and runs the build), the Java suites including the end-to-end suite against the real core, and the API smoke test against the jar that was just built |
| **engine** | The C++ engine build and its tests |

The commands in the workflow are the same ones documented above; run them locally to reproduce a CI
failure.

Pushing a tag such as `v0.2.0` runs [`.github/workflows/release.yml`](../.github/workflows/release.yml),
which publishes a GitHub release whose notes are the matching section of
[CHANGELOG.md](../CHANGELOG.md) (`.github/scripts/release-notes.sh`). A tag without a changelog entry
fails that workflow.

## Writing tests

- **A bug fix starts with a test that fails.** Put it at the lowest layer that can express the bug:
  a unit test if one function misbehaves, a behaviour test if it takes a hostile server to show it.
- **Behaviour tests** are plain functions named `test_*` in `tests/test_core.py`, registered in the
  `TESTS` list. Use `expect(condition, "what should be true")` for every check; give each scenario its own
  file names and download ids (`nid()`); stop every `Site` you start.
- **Unit tests** use the tiny macro framework at the top of each `test_unit*.c`; add them to the
  suite that already links the module.
- Keep tests deterministic: inject the clock where the code allows it (the host gate does), script the
  server instead of racing it, and avoid fixed sleeps.
- Run the layers you touched, then `bash tests/run_all.sh`, before you push.
