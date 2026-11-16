# Audit

Where each phase of the plan stands, what is left in it, what the audit found and fixed, and what the final pass
(hardening, refactoring, optimisation) has to do once the phases are finished. The [ROADMAP](ROADMAP.md) says
what comes next; this document says what state things are in and how that was checked. It is a snapshot: the
numbers are those of the commit that added it.

- [How to read this](#how-to-read-this)
- [At a glance](#at-a-glance)
- [Phase 1: the C core](#phase-1-the-c-core)
- [Phase 2: the C++ engine layer](#phase-2-the-c-engine-layer)
- [Phase 3: the Java daemon](#phase-3-the-java-daemon)
- [Phase 4: the video grabber](#phase-4-the-video-grabber)
- [Phase 5: the desktop UI](#phase-5-the-desktop-ui)
- [Phase 6: the browser extension](#phase-6-the-browser-extension)
- [Phase 7: build, install and polish](#phase-7-build-install-and-polish)
- [Across the phases](#across-the-phases)
- [What this audit could not check](#what-this-audit-could-not-check)
- [After the phases: harden, refactor, optimise](#after-the-phases-harden-refactor-optimise)
- [Decisions needed](#decisions-needed)
- [Register of open items](#register-of-open-items)

## How to read this

The phases are those of the project plan: 1 the C core, 2 the C++ engine layer, 3 the Java daemon, 4 the Python
video grabber, 5 the React and Electron UI, 6 the browser extension, 7 the build system. The plan gave each phase a
goal, a milestone and a list of files, and each section below compares the tree with all three.

Three words are used with care. **Checked** means a command was run on this commit's code and its output was read
(this commit changes only documents, so what ran on the one before it ran on the same code).
**Fixed** means a test that fails on the old code passes on the new; where no such test could be written, the entry
says so. **Not checked** is said wherever a check could not be run, with what stands in for it.

Every fix is a commit of its own, and its message holds the evidence. The [CHANGELOG](../CHANGELOG.md) lists what
a user would notice. Nothing here is a claim that the code has no defects left; each "no defect found" is limited
to what the named check can reach.

**How the checks were run**

| Check | Used on | In CI |
|---|---|---|
| Unit, integration and behaviour suites ([TESTING.md](TESTING.md)) | core | yes |
| AddressSanitizer + UBSan builds of the core and of every unit test; a ThreadSanitizer build of the core | core | yes (`core-sanitizers`) |
| Compiler warnings: the Makefile's set (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2` and more), then also `-Wswitch-enum -Wlogical-op -Wformat-truncation=2` | core | the first set |
| `gcc -fanalyzer`, `clang --analyze`, `clang-tidy` (`bugprone-*`, `clang-analyzer-*`, `concurrency-*`, `cert-*`) | core | no |
| Mutation fuzzers under ASan + UBSan, each checking properties of what the parser accepts: URL, redirect resolution, response head, IPC frame, resume file; the IPC socket; hostile and merely odd HTTP servers, with the property that every download ends | core | no: the harnesses are not in the repository yet |
| Mutation fuzzer under ASan + UBSan; Valgrind | engine | no |
| Harness suites against stand-ins for Spring and for the core, and against the real core | daemon | yes (`daemon`) |
| Jest, ESLint with no warnings allowed, the Electron shell's tests, the production build | UI | yes (`ui`) |
| `npm audit`, in full and with `--omit=dev` | UI | no |
| Node tests against a stand-in for `chrome.*`; Python tests over real pipes | extension | yes (`extension`) |
| `lizard` (function size and complexity), `readelf` on the built binary, a replay of the SQL files in SQLite | all | no |

## At a glance

| Phase | Plan's files | Milestone | State |
|---|---|---|---|
| 1. C core | 24 of 24 | Met, through the IPC rather than a command line | Complete. Three defects fixed, one of them a download that could be retried for ever. No memory-safety defect found. |
| 2. C++ engine layer | 13 of 17: no FTP, no QUIC | Not met: not linked into the core, and no BitTorrent client | Prototype. Ten defects fixed. |
| 3. Java daemon | 29 of 36: seven explained below | Met | Complete for what it does. One hardening and one test race fixed; schema management is unfinished. |
| 4. Python video grabber | 0 of 16: removed on purpose | Replaced: capture from the browser instead of an extractor | Designed ([VIDEO_CAPTURE.md](VIDEO_CAPTURE.md)), not built. |
| 5. React and Electron UI | 39 of 49: ten explained below | Met, except the video flow that was removed | Complete for the features it has. One flaky test fixed; Electron is out of support. |
| 6. Browser extension | 14 of 15 | Not met: nothing is handed over yet | Prototype. It used to lose every download; fixed. |
| 7. Build system | 0 of 7 | Not started | `dev_start.sh`, one test script per part and CI stand in. |

Checked on this commit:

| Suite | Checks | Result |
|---|---|---|
| Core unit tests (three binaries, always under ASan + UBSan) | 377 + 726 + 262 | pass |
| Core integration | 45 | pass |
| Core behaviour (`--quick`) | 391 | pass |
| Engine | 170 | pass |
| Daemon (five suites) | 989 | pass |
| UI: Jest | 405 in 35 suites | pass |
| UI: Electron shell | 50 | pass |
| Extension | 13 (Node) + 17 (Python) | pass |

What the audit changed, in the order the commits were made. Each is described in its phase below.

| Commit | Phase |
|---|---|
| `fix(ui): do not assert a transient link state in the live-feed test` | 5 |
| `fix(engine): validate untrusted .torrent and header input` | 2 |
| `fix(core): keep the resume file private and free of passwords` | 1 |
| `fix(daemon): accept only plain ASCII in the Host check` | 3 |
| `fix(extension): hand a download over before cancelling it` | 6 |
| `fix(core): keep passwords out of URL error messages` | 1 |
| `test(daemon): wait for the slot, do not race the completion handler` | 3 |
| `fix(engine): offer only pieces a peer's bitfield covers` | 2 |
| `fix(core): give up on a stream that never gets further` | 1 |
| `refactor(core): find the end of the authority without strcspn` | 1 |

## Phase 1: the C core

**Plan.** A daemon that downloads one file over HTTP or HTTPS in segments, resumes, and is driven over a socket.
Milestone: `./fluxdm-core "https://example.com/file.zip" --segments 16` downloads and resumes correctly.

**State.** All 24 planned files exist. The core no longer takes a URL on its command line: it is started once and
given downloads over the IPC, so the milestone is met through `START` with a segment count
([IPC_PROTOCOL.md](IPC_PROTOCOL.md)). The release binary on Ubuntu 24.04 is a PIE with RELRO, `BIND_NOW`, a stack
protector and fortified calls; the Makefile asks for the protector and fortification itself and gets the rest
from the compiler's defaults (P1-04).

**Checked.**

- The three unit binaries, the integration suite and the behaviour suite pass (counts above).
- The core built with ASan + UBSan passes the integration suite (45) and the behaviour suite (391), and its
  own log holds no sanitizer report; the ThreadSanitizer build passes the integration suite (45).
- Static analysis, on this tree. The Makefile's warning set: no warnings. Beyond it, 57: `-Wswitch-enum` 50,
  `-Wlogical-op` 4 and `-Wformat-truncation=2` 3 (a log label that cuts a path at 200 bytes on purpose, a `%.*s`
  into a buffer the caller sizes, and a socket path whose length is checked a few lines before the `snprintf`);
  all style. `gcc -fanalyzer` printed nothing. `clang --analyze` printed one finding, a false positive in
  `http_url_redact` (it does not know that `strcspn` stops at the terminator); `refactor(core): find the end of the
  authority without strcspn` removes it. `clang-tidy` with `bugprone-*`, `clang-analyzer-*`, `concurrency-*` and
  `cert-*` gave 234 findings, read by group: 122 are the advice to replace `memcpy` and `snprintf` with their
  Annex K forms; 49 are unused return values (`cert-err33-c`); 17 are calls POSIX does not require to be
  thread-safe, 13 of `strerror`, 2 of `getenv` and 2 of `exit`, which glibc makes safe as they are used here (a
  real `errno`, no `setenv` anywhere in the core, `exit` for `--help` before any thread exists); 15 are easily
  swappable parameters; 18 integer widening and narrowing; 6 assignments inside `if`; 2 `strcat` calls, each
  bounded by the length check on the line before; 2 about a `va_list` that the code does initialise; one `atoi`
  whose result is clamped; one function named with a leading underscore; and the false positive in
  `http_url_redact` that `clang --analyze` also reported. None was a defect.
- Fuzzing, on the code at this commit: 3,000,000 inputs to the parsers (URL 450,000; redirect resolution 450,000;
  IPC frame 450,000; resume file 450,000; response head 1,200,000), each with its properties checked, in
  exactly-sized heap blocks so that a read one byte past the end is an ASan error: no crash, no property violated.
  80,000 mutated and malformed frames sent to the ASan core's socket in two runs of 40,000: the core stayed alive
  and answering, with no sanitizer report.
- Hostile and merely odd HTTP servers, against the ASan + UBSan core: 900 downloads in three runs of 300 (seeds 31
  to 33, twelve at a time). 530 were honest but unusual: bodies cut into random pieces, chunked encoding, a body
  that ends with the connection (HTTP/1.0), a server without ranges, legal but odd headers (no reason phrase,
  mixed-case names, a repeated equal `Content-Length`, bare line feeds, a folded header), a flood of `100 Continue`
  and `103` responses before the real one, and chains of one to six redirects in five forms of `Location`. Every
  one completed with exactly the right bytes. 370 were hostile: bodies shorter or longer than promised, corrupt
  chunk framing, a lying `Content-Range`, headers of 8 to 70 KB, garbage, resets and a redirect loop. Every one
  reached an end (295 with an error, 75 complete), and none of the short, long, corrupt-chunk, oversized-header,
  garbage, reset or looping responses was reported complete with the wrong bytes (a lying `Content-Range` is served
  with the right bytes, so there only an end was required). The core stayed alive and answering, wrote no
  sanitizer report and exited cleanly when asked to. An earlier version of this run, before the fix in item 3
  below, had 8 downloads without an end in 600. (Sixteen clients at a time exceed the core's own limit of 16 IPC
  clients, so the runs use twelve.)

**Found and fixed.**

1. *The resume file.* `fix(core): keep the resume file private and free of passwords`. The `.fluxpart` file was
   created with the process's umask (mode 0666 under a umask of 0), followed a symlink placed at its path (the
   file the link pointed to was overwritten), blocked the core on a FIFO placed at its path, and held the URL's
   password in clear text. It is now created `0600` with `O_EXCL` and `O_NOFOLLOW`, read only when it is a regular
   file, and written with the URL's userinfo removed; a resume compares the two redacted URLs. Unit checks went from
   272 to 343. On the old code 25 of the new unit checks fail and the run then hangs on the FIFO, and 10 of the
   91 behaviour checks selected by `-k resume` fail.
2. *Passwords in error messages.* `fix(core): keep passwords out of URL error messages`. A URL the core refused,
   for example `http://alice:hunter2@/x`, was quoted whole in the error it returned, which the daemon logs and
   stores as the download's error message. Messages now quote the URL without its userinfo
   (`http_url_redact`). Unit checks went from 343 to 377; the behaviour suite's basic-auth test gained 8 checks,
   4 of which fail on the old core.
3. *A stream that never got further was retried for ever.* `fix(core): give up on a stream that never gets
   further`. A server that cannot serve ranges can only stream from byte 0, so when its stream breaks, the segment
   starts again from zero and drops what it had received. The retry budget (8) was refilled whenever an attempt got
   16 KiB past where the last one ended, and a restart set that point back to zero, so every attempt looked like
   progress however often it covered the same bytes. A server that always cut the stream at the same place kept
   the download reconnecting every half second and the download never failed. Found by the hostile-server fuzzer,
   whose property is that every download ends: 8 of the 600 downloads in two runs of 300 (5 and 3) had no end
   after 130 s.
   Reproduced against a local server without ranges that cuts the stream at 200,000 bytes: the old core made 220
   requests in 100 s, every one "retry 1 in 455 ms", and was still going; the new one gives up after 9 requests, in
   65 s, with "gave up after 8 retries". Progress now has to get past the furthest point any attempt reached.
   A server that can resume is unaffected, and a test checks it: one cut 12 times, more often than the budget
   allows, is still followed to the end. The behaviour suite gained 6 checks (the test takes 70 s, nearly all of it
   back-off); 3 fail on the old core.

**Found and left.** None of these is a defect today.

- The connection pool (`flux_pool_*` in `connection.c`) is compiled and nothing calls it (P1-01).
- An absolute redirect target is used as given; relative ones are resolved and have their dot segments removed.
  Servers normalise either way (P1-02).
- A `Content-Range` whose last byte is at or past the total is accepted. The core bounds what it writes by its own
  ranges, so this changes nothing it stores (P1-03).
- A server that cannot resume, and whose stream breaks, makes the core fetch the file again from byte 0, up to 9
  times in about a minute before it gives up. That cannot be avoided without ranges; what the user sees is the
  progress bar falling back to zero.

**Left to do in this phase.** Feature work, in the [ROADMAP](ROADMAP.md#next): a "waiting for server" state (1),
per-segment progress (2), request headers in `START` (3), validation on every segment (6). Later: connection reuse
and TLS session resumption, proxy support, persistent host knowledge.

## Phase 2: the C++ engine layer

**Plan.** A segment manager that feeds work to the C core, an adaptive tuner that adjusts the connection count, a
torrent engine for magnets and `.torrent` files, and FTP and QUIC modules in the core. Milestone: a magnet link
downloads, and the tuner speeds up fast CDNs by itself.

**State.** Thirteen of 17 files: `SegmentManager`, `AdaptiveTuner`, `BufferManager`, `MetadataParser`,
`TorrentEngine`, `PeerDiscovery` and the build files. `ftp.c`, `ftp.h`, `quic.c` and `quic.h` were never written;
the limitations table already says "HTTP/1.1 and HTTPS only".

The milestone is not met, for two reasons. The engine is not linked into the core or the daemon, so nothing feeds
it work and nothing reads what it decides. And it is not a BitTorrent client: it parses `.torrent` files and
magnet links, hashes pieces with SHA-1, reads compact peer lists and talks to trackers over HTTP and UDP, but has
no peer wire protocol, and DHT announce and lookup are marked `NOT IMPLEMENTED` in the code. The ROADMAP says the
same ("a separate, large piece of work").

**Checked.**

- 170 checks pass. The engine is compiled with the core's objects and run by `tests/run_engine_tests.sh`.
- A mutation fuzzer under ASan + UBSan over bencode, `.torrent` structure, base32 and file-name handling, and
  Valgrind over the tests. The 170 checks also pass under ASan + UBSan. The fuzzer ran two seeds of 500,000 mutated
  inputs for each of four targets (bencode, base32, the `Content-Disposition` file name and whole `.torrent`
  structures: 4,000,000 inputs), and nesting up to a million levels deep. Nothing crashed and no property was
  violated: what the bencode parser accepts re-encodes and parses to the same value; a file name is one plain
  component of at most 255 bytes without control characters; the paths in a torrent are relative and do not climb
  out of the download folder. Valgrind over the 170 tests reports no error and no leak.
- The tests cover compact-peer parsing and PEX; the tracker requests themselves (HTTP and UDP announce) are not
  exercised by any test (P2-07).

**Found and fixed.**

1. *Untrusted `.torrent` and header input.* `fix(engine): validate untrusted .torrent and header input`. Nine
   problems, listed in the CHANGELOG: a `.torrent` nested 100,000 levels deep and a `Content-Disposition` file name
   of 30 KB or more each overflowed the stack; a string length near 2^64 wrapped past a bounds check; numbers such
   as `i+5e` and `i5xe` were read as 5; a path component such as `../x` was accepted; the info-hash was taken from a
   re-encoded copy of the dictionary, so it was wrong whenever the keys were not already sorted; the last piece was
   always given the full piece length; some fields were never initialised; the base32 decoder shifted a signed
   value. The core refuses a response head above 16 KB, so the file-name overflow could not be reached through it.
   Checks went from 86 to 149; against the old code the new ones crash the process, and 31 of the others fail.
2. *A short bitfield was read as "has the rest".* `fix(engine): offer only pieces a peer's bitfield covers`.
   `next_piece` skipped its check for every piece past the end of the peer's bitfield, so an empty bitfield answered
   piece 0 and a three-entry one answered piece 3 once pieces 0 to 2 were done. 21 new checks (149 to 170); 4 of
   them fail on the old code.

**Found and left.**

- "Rarest first" is lowest index first: `piece_availability_` is set to zero at load and nothing updates it. Noted in
  the header (P2-02).
- Peer IDs are seeded from `time(nullptr)` into a 32-bit Mersenne Twister, and UDP transaction IDs and DHT node IDs
  from a 32-bit `random_device` seed. None of these needs to be secret, but all can be predicted; `getrandom` would
  do (P2-03).

**Left to do in this phase.** Decide what the engine is for (see [Decisions needed](#decisions-needed)). If it is
kept: link it into the core through a narrow C interface, check a bitfield's length where it is received (the wire
format fixes it, and a client should drop a peer that sends another), count piece availability, and test the
trackers against a local one. FTP and QUIC: drop from the plan or schedule after 1.0.

## Phase 3: the Java daemon

**Plan.** A process that manages the queue, exposes a REST API on `localhost:7171` and sends live updates to the
UI, and that starts on boot. Milestone: `curl localhost:7171/api/downloads` returns JSON, a WebSocket streams speed
updates, queue priorities work, the scheduler fires downloads at set times.

**State.** 29 of 36 files. The seven that are absent:

| Planned | Why it is absent |
|---|---|
| `GrabberController`, `GrabberService`, `PythonBridge` | Removed with the grabber (phase 4) |
| `db/DatabaseManager`, `config/AppConfig` | Spring Data JPA and Spring Boot's configuration do their work |
| `model/Segment` | Segments are not persisted, by design: the core tracks them (`db/schema.sql` says so) |
| `ipc/schema.json` | Never written. The protocol is defined by `ipc/protocol.h` and `ipc/messages.h` and described in [IPC_PROTOCOL.md](IPC_PROTOCOL.md) (P3-08) |

The milestone is met, with two notes: the API now needs the token (`curl -H "Authorization: Bearer $(cat
~/.fluxdm/api-token)"`), and "starts on boot" belongs to phase 7 (a `systemd --user` unit does not exist yet).

**Checked.**

- 989 checks in five suites: the API's guard (379), the queue (45), the IPC client (110), the download service
  (284) and an end-to-end suite against the real core (171). Twelve rounds of the four fast suites, 48 runs, all
  passed.
- Reading the daemon for log statements that print a URL: none does, except through a core message, which phase 1
  now redacts.

**Found and fixed.**

1. *The `Host` check normalised what it should have refused.* `fix(daemon): accept only plain ASCII in the Host
   check`. `ApiGuard` trimmed and lower-cased the header with Java's Unicode-aware functions, so ` localhost`
   (an em space), `localhost ` and `　127.0.0.1:7171` were accepted as loopback names, and the Kelvin sign
   (U+212A) lower-cased to `k`, so a configured name such as `desktop` also matched `desKtop`. No way to use
   this was found: browsers do not send such a header and the check still separates loopback from everything else.
   It is hardening. The check now accepts printable ASCII only, after trimming spaces and tabs. Checks went from 364
   to 379; 7 fail on the old guard.
2. *A race in the tests, not in the service.* `test(daemon): wait for the slot, do not race the completion handler`.
   The full daemon run of this audit failed once in `DownloadServiceTest` ("slot given back", expected 0, got 1)
   and passed on the next. `onComplete` and `failDownload` update the row and then give the slot back, inside one
   `synchronized (stateLock)` block; a reader without that lock, as the tests are, can land between the two
   statements. With a 30 ms pause between them in a scratch copy, the old tests failed at that check and at five
   in the end-to-end suite; the new ones pass there, and still fail when the slot is never given back. The service
   is unchanged. (`getStats` reads its counts without the lock too; the gap is a few microseconds and the result is
   a display.)

**Found and left.**

- The SQL under `db/` and `daemon/src/main/resources/schema.sql` cannot be used, and nothing uses it. Replayed in
  SQLite: `001_init.sql` holds comments only (its header says "applied automatically on first run"; nothing applies
  it), so `003_categories.sql` fails with `no such table: downloads` and the seeds with `no such table:
  categories`. `daemon/src/main/resources/schema.sql` holds comments only as well, and says "auto-applied by Spring
  on startup". `db/schema.sql` differs from what the daemon stores: it has `created_at` and `updated_at`, which the
  `Download` entity does not have, and lacks `bandwidth_limit` and the `schedule_tasks` table, which it does. The
  schema the daemon uses is whatever Hibernate makes of the entities (`spring.jpa.hibernate.ddl-auto=update`). Not
  checked: that Spring really would not run `schema.sql` (it needs Maven, below) (P3-01, P3-02).
- `model/Category` is an entity nothing uses; `ddl-auto=update` would create its table (P3-03).
- The default log level is `DEBUG` for `com.fluxdm` (P3-04), and the default core path is
  `~/Desktop/fluxdm/core/fluxdm-core` (P3-05).
- A URL with a password is stored as given, in a database under `~/.fluxdm`, which is private to the user
  (P3-06).
- The Java side of the IPC is not checked against the C headers by a test that reads them; the end-to-end suite
  runs the real core, which covers behaviour (P3-09).
- The STOMP broker sends no heartbeats (ROADMAP).

**Left to do in this phase.** A schema strategy (see [Decisions needed](#decisions-needed)), the defaults above,
and starting on boot (phase 7). The daemon was built with `javac` against stand-ins; see [What this audit could not
check](#what-this-audit-could-not-check).

## Phase 4: the video grabber

**Plan.** A Python service that, given a page address, returns a video's title and formats using yt-dlp, and hands
the direct URL to the core. Sixteen files under `grabber/`.

**State.** None of them exists, and that is the decision, not a gap: the extractor was removed
(`feat(daemon)!: remove the grabber service and its yt-dlp bridge` and the UI's counterpart) in favour of taking
what the browser itself plays, the way a download manager's extension does. The design is
[VIDEO_CAPTURE.md](VIDEO_CAPTURE.md), with formulas that `tests/capture_model_check.py` checks (20 checks) and
`tests/mutation_capture.py` breaks (23 mutations) in CI. The plan's milestone (`POST /grab` returns title and
formats) no longer applies; its replacement is the [build order](VIDEO_CAPTURE.md#13-build-order).

**Left to do in this phase.** All of the build order: request headers in `START` (ROADMAP 3), the extension's hand-off
and capture (4), segment jobs in the core and daemon (5), and the measurements of
[section 14](VIDEO_CAPTURE.md#14-what-has-to-be-measured), among them the real-browser check that a Manifest V3
`webRequest` listener sees `Cookie` and `Referer`, and the decision whether to depend on `ffmpeg` for joining
tracks. `build_grabber.sh` from phase 7 is obsolete.

## Phase 5: the desktop UI

**Plan.** A desktop app with the download list, a speed graph, an add dialog, a video grabber view, a scheduler and
settings, all updating live. Milestone: the app launches, shows live downloads, the graph animates, the video flow
works end to end.

**State.** 39 of 49 files. Of the ten that are absent, five went with the grabber (`api/grabber.js`,
`Grabber/SiteCard.jsx`, `Grabber/FormatPicker.jsx`, `Grabber/VideoGrabber.jsx`, `Modals/GrabModal.jsx`) and five are
replaced by modules named otherwise and tested: `store/statsSlice.js` (totals are derived from the downloads),
`hooks/useWebSocket.js` (`api/live.js`, `store/liveSync.js` and `hooks/useLiveSync.js`), `hooks/useDownloads.js`
(selectors in `store/downloadsSlice.js`), `hooks/useSpeedData.js` (`hooks/useSpeedHistory.js`) and `main.jsx`
(`index.js`). The milestone is met except for the video flow. Settings has a global speed limit, a default
download folder and a default segment count; the Appearance tab is a placeholder that says "more themes coming in
Phase 5" (P5-04).

**Checked.**

- ESLint with no warnings allowed, 405 Jest tests in 35 suites, 50 tests of the Electron shell, and the production
  build, in 31 seconds.
- Five more rounds of the Jest suite with both CPUs fully loaded by busy loops: all five passed.
- Everything the daemon sends passes through one place before a component sees it: `api/downloads.getAll`
  validates the list, `api/live.js` accepts only objects with a string `type`, and `model/downloads.fromDaemon`
  coerces every field. A malformed payload is dropped or normalised, not rendered.
- `npm audit`: nothing in the 95 production packages (`--omit=dev`). That run leaves Electron out, because it is a
  development dependency, although the packaged app carries it. In all, 37 packages are flagged (15 high, 6
  moderate, 16 low; 74 advisories between them): 35 through `react-scripts`, which are build and test tools that
  the packaged app does not contain, and 2 through Electron, Electron itself and its installer's `extract-zip`.
  Against Electron 29.4.6 alone npm lists 45 advisories and offers 45.3.2, a major move (P5-01).
- The built app, run by hand on Ubuntu, launches, connects and shows live downloads. That run is the author's, not
  one this repository can repeat (P5-11).

**Found and fixed.**

1. *A flaky test.* `fix(ui): do not assert a transient link state in the live-feed test`. While nothing listens, the
   feed alternates between "connecting" and "offline" on every retry, so which of the two a test sees right after
   `waitFor` returns depends on timers. It failed in a real run. The behaviour is by design and unchanged; the test
   now asserts what does not depend on timing. With the retry delay forced to 1 ms the old assertion failed 150 of
   150 times and the new one passed 150 of 150.

**Found and left.**

- Electron 29 is out of support, and so is the toolchain that builds the UI (`react-scripts`) (P5-01, P5-02).
- There is no React error boundary: an exception while rendering, from whatever cause, blanks the window and
  nothing offers a reload (P5-03).
- The categories sidebar (`CategoryList.jsx`), `tray.js` and three API wrappers are unused; the `categories` feature
  exists in the daemon's model, the SQL seeds and the sidebar, and nowhere as a working feature (P5-05).
- Styling lives in long inline `style` props; the colour tokens are CSS variables but the layout is not (P5-06).
- Also listed in the ROADMAP's audit candidates: the default menu with Developer Tools, contrast of dim text, one
  shared modal with a focus trap, stale rows when the daemon is lost.

**Left to do in this phase.** The items above; an end-to-end job that starts the built daemon and drives the built
page; a folder picker; a packaged build with an icon.

## Phase 6: the browser extension

**Plan.** An extension that catches every download in the browser and reroutes it to FluxDM, with a detector that
adds a "Grab with FluxDM" button on video sites. Milestone: a download in Chrome opens FluxDM with a segment
selector; a YouTube page shows the button.

**State.** 14 of 15 files; `background/native_bridge.js` was removed on purpose (its job is in `background.js`).
The milestone is not met and was replaced: the video button gives way to capture from the browser
([VIDEO_CAPTURE.md](VIDEO_CAPTURE.md)), and nothing is handed over yet because the native-messaging host cannot
pass a download on. Three one-line placeholder files remain (`background/interceptor.js`, `content/content.js`,
`content/video_detector.js`).

**Checked.** 13 Node tests against a stand-in for `chrome.*` and 17 Python tests over real pipes; an `extension`
job in CI runs both.

**Found and fixed.**

1. *The extension lost every download.* `fix(extension): hand a download over before cancelling it`. It cancelled
   every download the browser started and handed it to nothing: its host answered `ok` to every message, and
   nobody was listening to the extension's own announcement. It now offers each download to the host and cancels
   the browser's copy only when the host answers `accepted`; no host, a crashing, silent or failing host, or any
   other reply leaves the download with the browser. Private windows are never offered. The host does not answer
   `accepted` until it can pass a download on, validates what it receives, answers bad JSON instead of exiting (it
   quit on `{}`, `[]` and `0`), and refuses frames over 64 KB instead of trusting the length it was told. The
   manifest asks for `downloads` and `nativeMessaging` only; `declarativeNetRequest`, `storage` and access to every
   site were unused, and an empty script was injected into every page. The old script fails all 5 tests that can run
   against it, and the old host 13 of 17.

**Found and left.**

- Today `eligible()` offers loopback and private-network addresses to the host (a test says
  `http://127.0.0.1:7171/x` is offered). It is harmless only because the host cannot hand anything over.
  [VIDEO_CAPTURE.md](VIDEO_CAPTURE.md#11-the-hand-off-is-a-trust-boundary) already has the rule (a loopback or
  private target needs the person's confirmation); the first version of the hand-off must apply it (P6-02).
- `native_host.json` still holds a placeholder path and extension ID, so a person cannot install the host yet
  (P6-03).
- Chromium only: `background.service_worker` is not accepted by Firefox, which wants `background.scripts` (P6-04).
- The popup shows a constant "Ready" and does not reflect the host's state (P6-05).

**Left to do in this phase.** ROADMAP 3 and 4: headers in `START`, the host passing a download to the daemon with the
API token, progress in the popup, then capture.

## Phase 7: build, install and polish

**Plan.** One command builds and installs everything: seven scripts under `build/` (`build_core.sh`,
`build_engine.sh`, `build_daemon.sh`, `build_grabber.sh`, `build_ui.sh`, `build_all.sh`, `install.sh`).
Milestone: `./build/build_all.sh && ./build/install.sh` leaves FluxDM installed, the extension loaded and every part
talking to the others.

**State.** Not started: there is no `build/` directory. What stands in: `dev_start.sh` starts the parts from a
checkout; `tests/run_*_tests.sh` builds each part; CI builds each part on its own. Nothing builds a release
artifact, nothing installs, and nothing proves that FluxDM runs on a machine that is not the author's.

**Left to do in this phase.** The whole of it: build scripts and a `build_all.sh`; `install.sh` with an uninstall and an
upgrade path that keeps the database; a `systemd --user` unit for the daemon (which also closes phase 3's "starts
on boot"); a `.deb`; the native-messaging manifest installed with the right path and extension ID; an icon and
`chrome-sandbox` set up for the packaged app; and a clean-machine job in CI that installs from the artifact and runs
the smoke test. `build_grabber.sh` is obsolete.

## Across the phases

**Where a URL's password can end up.** The core's log never prints a URL's userinfo (`log_url`). The core's error
messages did (fixed). The resume file did (fixed). The daemon logs core messages and nothing else that carries a URL,
so it is clean now. The daemon's database stores the URL as given (P3-06). The UI shows the host of a download, not
the address.

**Defaults that suit development and not a release.** `DEBUG` logging, `ddl-auto=update`, a core path under one
person's desktop, and an Electron menu with Developer Tools.

**Stubs, placeholders and dead code, in one list.** `flux_pool_*`; `model/Category`, the `categories` SQL and
`CategoryList.jsx`; the SQL under `db/` and `schema.sql` in the daemon's resources; `electron/tray.js`; three API
wrappers; `AppearanceSettings`; three one-line files in the extension; the engine layer as a whole until it is
used.

**What the CI does not yet do.** Build or install a release; run a fuzzer; measure coverage; run `clang-tidy`,
`cppcheck`, SpotBugs, a Python linter or `npm audit`; check dependencies for advisories on the Java side; pin its
actions by commit instead of tag.

**Text that said something untrue.** Two SQL headers (above), the Appearance tab's "Phase 5", and, before this
audit, the CHANGELOG's account of the engine's input handling and the extension's behaviour; those were corrected
with the fixes.

## What this audit could not check

- **Spring.** The sandbox could not reach Maven Central (HTTP 403), so `mvn verify`, the JPA mappings against SQLite,
  the controllers' routes, the SockJS and STOMP endpoints and `tests/smoke_api.sh` were not run. The daemon's
  logic was run with `javac` against stand-ins for Spring; CI's `daemon` job builds and tests the real thing.
- **Electron.** The sandbox has no Electron binary. The shell's rules were tested against a stand-in; a real run was
  made by the author.
- **Real browsers.** The extension was tested against a stand-in for `chrome.*`, not in Chrome, Edge, Brave or
  Firefox.
- **Other systems.** Ubuntu 24.04 on x86-64 only, with gcc 13.3 and clang 18.1.3. No other distribution, libc,
  architecture or kernel.
- **Real networks.** The kernel has no `tc netem`, so nothing was tested over shaped, lossy or high-latency links,
  and nothing against public servers. The core was exercised over loopback against stand-in servers, including
  hostile ones, and TLS against a local server.
- **Time.** No soak test: nothing ran for days, and leaks were looked for only by ASan's check at exit.
- **Fuzzing depth.** The fuzzers mutate and check properties; none is coverage-guided (no libFuzzer or AFL++ here), so
  deep states are less likely to be reached than in a coverage-guided run. Millions of inputs and minutes of CPU, not
  days.
- **Review.** No third party has read the code.

## After the phases: harden, refactor, optimise

The order is deliberate. Hardening comes first because it adds the tests, fuzzers and gates that make a refactor
safe. Refactoring comes second because optimising clean seams is cheaper and a benchmark of tidy code is not
thrown away. Optimisation comes last and only on measurements: no target below is a number yet, because none has
been measured.

**Entry.** The pass starts when phases 4, 6 and 7 have delivered their features (so that what is hardened is what
ships), the engine's fate is decided, and the install works on a clean machine. Features stop while it runs.

### Harden

| | Work | Exit criterion |
|---|---|---|
| H1 | Commit the fuzz harnesses as `tests/fuzz/`: libFuzzer targets for the URL parser, redirect resolution, response head, chunk decoder, IPC frame decoder, resume loader, bencode, `Content-Disposition` and the native host's framing, and, once they exist, HLS and DASH manifests (the largest new untrusted input, XML included). Seed corpora. The hostile-server run as a test of liveness as well as of safety: every download against any server ends within a stated bound (that is how the retry-forever defect was found, by 8 downloads out of 600 that did not). A 60-second smoke run per pull request and a longer nightly run. | Every target runs an hour without a finding before each release; a crash or a download that does not end fails CI |
| H2 | Static gates in CI: `clang-tidy` with a curated check list, `cppcheck`, `-Wconversion` where practical, `-Xlint:all -Werror` and SpotBugs with FindSecBugs for Java, ESLint's security rules for JS, `ruff` and `bandit` for the native host. Sanitizers on the engine too. | Zero warnings with the chosen set; every suppression carries a reason |
| H3 | Supply chain: `npm audit --omit=dev --audit-level=high` as a gate, with Electron counted as well, since it ships although it is a development dependency; OWASP dependency-check for the Maven side; Dependabot or Renovate; actions pinned by commit; lockfiles verified; install scripts restricted to an allow-list when the npm in use can (of the 1,423 packages in the lockfile only Electron's is needed); an SBOM per release. | No known high or critical advisory in anything that ships; updates arrive as pull requests |
| H4 | Electron: a supported major; fuses set (`RunAsNode` off, `OnlyLoadAppFromAsar` on, ASAR integrity on); `sandbox`, `contextIsolation` and the CSP asserted by the shell's tests; a menu without Developer Tools in a packaged build. | The shell's mutation test passes against the new Electron |
| H5 | Daemon: `INFO` by default with a log filter that redacts URLs; Flyway migrations and `ddl-auto=validate`; a decided policy for credentials in URLs; request, frame and connection limits; STOMP heartbeats and a message size limit; security headers on API responses; a clean stop of the core on shutdown. | `ApiSecurityTest` and `smoke_api.sh` cover each |
| H6 | Core: the hardening flags stated in the Makefile (`-fPIE -pie`, `-Wl,-z,relro,-z,now`, `-D_FORTIFY_SOURCE=3` where the compiler has it) and checked on the built binary in CI; `PR_SET_NO_NEW_PRIVS`; an optional seccomp allow-list; limits on downloads, segments and open files; free-space checks before `fallocate`. | `checksec`-style check green in CI |
| H7 | Failure injection: kill each part at random moments and check recovery and file integrity; disk full (`ENOSPC`), short writes and `EIO` through a preload shim; clock jumps; a missing or read-only download directory. | A nightly chaos job passes |
| H8 | The threat model in [SECURITY.md](../SECURITY.md) rewritten around the real boundaries: page, extension, native host, daemon, core, network, disk. | Each boundary lists its assets, its attackers and the test that covers them |

### Refactor

The first list is measured. `lizard` on the production code at this commit finds 41 functions over a cyclomatic
complexity of 15 or 100 lines, 25 of them in the core:

| Part | Functions | Average complexity | Over the limit |
|---|---|---|---|
| core (5,013 lines of code) | 256 | 6.7 | 25 |
| engine (2,084) | 127 | 4.5 | 8 |
| daemon (2,501) | 263 | 2.7 | 3 |
| UI and shell (2,187) | 302 | 2.6 | 5 |
| extension (123) | 21 | 3.5 | 0 |

The worst, by complexity: `http_parse_head` 64, `http_url_parse` 48, `extract_info` (engine) 46, `flux_strerror` 38 and
`flux_ipc_wire_error` 29 (both `switch` tables, harmless), `dl_probe` 33, `seg_pump`, `seg_headers_complete` and
`seg_chunk_decode` 31 to 32, `validated` (daemon) 30, `probe_attempt`, `http_url_resolve`, `parse_content_range` and
`reconcile` (daemon) 27. `core/src/downloader.c` is 1,640 lines and holds eight of the core's 25. The parsers among
them are the ones the fuzzers reach, which is the safety net for splitting them.

| | Work | Exit criterion |
|---|---|---|
| R1 | Delete what nothing uses (the list under [Across the phases](#across-the-phases)), proved by a tool and not by eye: `knip` for JS, `-Wunused` and a symbol check for C, `vulture` for Python. | The tools report nothing |
| R2 | One source of truth: the schema (Flyway), the protocol constants (generate the Java and the docs from `ipc/messages.h`, or test them equal), one version number feeding every manifest. | A test fails when two of them disagree |
| R3 | A complexity budget: no function above a cyclomatic complexity of 15 or 100 lines, except tables of cases, listed. Start with the parsers and `downloader.c`; split the latter into probe, plan and run. | `lizard` in CI with the budget; the fuzzers and suites unchanged and green |
| R4 | UI: one modal with a focus trap; styles out of the JSX; the Vite move; `tsc --checkJs` or TypeScript for the model; an error boundary; an accessibility pass with automated checks. | Lint, tests and an `axe` run in CI |
| R5 | Daemon: separate API objects from entities if they are not; one error-handling path. | No entity is serialised directly |
| R6 | Engine: link it behind a narrow `extern "C"` interface, or remove it. | Decided in the [decisions](#decisions-needed) |

### Optimise

Measure first, with the benchmark of ROADMAP item 7 (a namespace with controlled bandwidth, delay and loss; at least
20 interleaved runs; medians with bootstrap intervals; Mann-Whitney with Cliff's δ). The baseline covers:
time to the first byte; throughput against curl, wget and aria2 across file sizes, delay, loss and per-connection
caps; CPU time per gigabyte; resident memory of each process with 1, 100 and 1,000 downloads; the daemon's time from
start to a served API; the latency of list and get with 1,000 and 10,000 rows; the UI's frame time with a hundred
progress events a second; the size of the UI bundle; idle wake-ups and CPU, since a download manager is mostly idle.

Candidates, in the order the data is likely to rank them, each kept only if it earns its place:

- Core: connection reuse and TLS session resumption; dynamic rebalancing of segments; buffer sizes from the
  bandwidth-delay product; the write path (coalescing, `fdatasync` policy); no timer work while idle.
- Daemon: start-up time (lazy initialisation, class-data sharing, trimmed auto-configuration), a heap cap, SQLite in
  WAL mode, progress persisted in batches, STOMP events coalesced.
- UI: a virtualised list, memoised selectors, progress events batched per frame, charts loaded on demand, Electron's
  start-up.

**Exit.** An optimisation lands only with a benchmark whose interval excludes zero and whose effect is not small, no
regression in any hardening gate, and a note in the code where the reason is not obvious. A nightly job compares each
run with the stored baseline and fails on a regression beyond the noise.

### Done means

A fresh Ubuntu 22.04 and 24.04 install can build, install, upgrade (keeping the database) and uninstall; every
suite passes under the sanitizers and the fuzz smoke runs; nothing that ships has a known high advisory and Electron
is supported; nothing in the tree is dead or a stub; every command in the README is run by CI; the benchmark's
numbers are published with their intervals; and [SECURITY.md](../SECURITY.md) says what is defended, against whom,
and by which test.

## Decisions needed

Each has a recommendation; none is made without you.

1. **The engine layer.** Wire in, freeze, or remove. Recommendation: do not ship it as it is. Integrate
   `SegmentManager` and `AdaptiveTuner` into the core's rebalancing only if the benchmark shows a gap they close;
   take BitTorrent out of the 1.0 scope.
2. **FTP and QUIC.** Planned in phase 2, never written. Recommendation: drop both from the plan for 1.0. Neither is
   in the limitations table as coming.
3. **Schema management.** Flyway with `ddl-auto=validate` (recommended: upgrades need real migrations), or Hibernate
   only and delete `db/`.
4. **Categories.** Implement them across the daemon, the SQL and the UI, or delete the entity, the seeds and
   `CategoryList.jsx`. Recommendation: delete until there is a use for them.
5. **Credentials in URLs.** The daemon needs the password to resume after a restart, because the core no longer
   stores it. Either keep the URL as given in the database, which sits in a private directory, or store it redacted
   and ask again after a restart. Both are defensible; the first is what happens today.
6. **Browsers.** Chromium only for 1.0, or Firefox as well (a second manifest and a test in each).
7. **Developer Tools.** Available in a packaged build or not.

## Register of open items

Class: **Blocker** means no release should ship with it open; **1.0** means needed before 1.0 and not before an
earlier release; **Polish** means worth doing and harmless to postpone. Size is an estimate: S is hours, M a day or
two, L a week or more.

| ID | Class | Size | Item |
|---|---|---|---|
| P1-01 | 1.0 | S | Connection pool compiled and never called: use it (connection reuse) or delete it |
| P1-02 | Polish | S | Absolute redirect targets are used as given |
| P1-03 | Polish | S | `Content-Range` with a last byte at or past the total is accepted |
| P1-04 | 1.0 | S | State PIE, RELRO and `BIND_NOW` in the Makefile and check the binary in CI; today they come from the compiler's defaults |
| P2-01 | 1.0 | L | The engine is not linked into anything: decide, then wire it in or remove it |
| P2-02 | 1.0 | M | `piece_availability_` is never updated, so "rarest first" is lowest index first |
| P2-03 | Polish | S | Peer, transaction and node IDs come from a 32-bit seed (and the peer ID from the clock); use `getrandom` |
| P2-04 | 1.0 | L | DHT announce and lookup are not implemented |
| P2-05 | 1.0 | L | No peer wire protocol: BitTorrent is a separate project |
| P2-06 | Polish | S | FTP and QUIC were planned and not written: drop from the plan |
| P2-07 | 1.0 | M | Tracker requests (HTTP, UDP) have no test |
| P3-01 | 1.0 | M | The SQL under `db/` does not build the schema and contradicts the entities |
| P3-02 | 1.0 | M | `ddl-auto=update` decides the schema at start-up; there is no upgrade path |
| P3-03 | Polish | S | `Category` is an unused entity |
| P3-04 | 1.0 | S | Default log level is `DEBUG` |
| P3-05 | Blocker | S | Default core path is `~/Desktop/fluxdm/core/fluxdm-core` |
| P3-06 | 1.0 | M | A URL's password is stored as given in the database |
| P3-07 | Polish | S | No STOMP heartbeats |
| P3-08 | Polish | S | `ipc/schema.json` was planned and never written |
| P3-09 | 1.0 | M | Nothing checks the Java IPC constants against `ipc/*.h` |
| P3-10 | Blocker | M | Not checked in the sandbox: the Spring wiring; keep CI's `daemon` job required |
| P3-11 | Blocker | M | The daemon does not start on boot (phase 7's unit) |
| P3-12 | Polish | S | What the daemon answers to SockJS's credentialed `GET /ws/info` from the development server's origin (`http://localhost:3000`) is unchecked; only the built app's `app://-` was run by hand (ROADMAP) |
| P4-01 | 1.0 | L | The whole capture build order (ROADMAP 3, 4, 5) and its measurements |
| P5-01 | Blocker | L | Electron 29 is out of support; recharts 2 is deprecated (move to 3) |
| P5-02 | 1.0 | L | `react-scripts` is unmaintained and carries 35 of the 37 packages `npm audit` flags, all build and test tools: move to Vite |
| P5-03 | 1.0 | S | No React error boundary |
| P5-04 | Polish | S | The Appearance tab is a placeholder that mentions "Phase 5" |
| P5-05 | Polish | S | Unused: `CategoryList.jsx`, `tray.js`, three API wrappers |
| P5-06 | Polish | M | Styles are inline in the JSX |
| P5-07 | Polish | M | Contrast of dim text, shared modal, focus return (ROADMAP) |
| P5-08 | 1.0 | S | The packaged menu has Developer Tools (ROADMAP) |
| P5-09 | Polish | M | No folder picker; scheduling needs a paused or failed download, and a repeating schedule drifts by an hour across a daylight-saving change (ROADMAP) |
| P5-10 | 1.0 | M | No packaged build, icon or `chrome-sandbox` setup |
| P5-11 | 1.0 | M | No end-to-end job: the daemon and the built page together |
| P5-12 | Polish | S | When the daemon is lost, the rows keep showing the last state it reported and only the banner says the connection is gone: mark them stale (ROADMAP) |
| P6-01 | 1.0 | L | The hand-off is not built: the extension does nothing yet (ROADMAP 4) |
| P6-02 | Blocker | M | Apply the hand-off rules of VIDEO_CAPTURE section 11, loopback and private addresses first, together with the hand-off |
| P6-03 | Blocker | M | The native-messaging manifest holds a placeholder path and extension ID |
| P6-04 | Polish | M | Chromium only |
| P6-05 | Polish | S | The popup shows a constant "Ready" |
| P6-06 | Polish | S | Three one-line placeholder files |
| P7-01 | Blocker | L | No build scripts, installer, unit, package or clean-machine test |
| P7-02 | Polish | S | `build_grabber.sh` is obsolete |
| X-01 | 1.0 | M | CI has no fuzzing, coverage, `clang-tidy`, `cppcheck`, SpotBugs, Python lint, `npm audit` or Java advisory check |
| X-02 | Polish | S | Actions are pinned by tag, not by commit |
| X-03 | 1.0 | M | The audit's fuzz harnesses are not in the repository (H1) |
| X-04 | Polish | S | The version number is repeated by hand in each part |
