#!/usr/bin/env python3
"""
FluxDM — tests/test_core.py
Behavioural tests for the C core daemon, driven through the real IPC protocol
against scriptable local HTTP(S) servers.

  FLUXDM_CORE=/path/to/fluxdm-core  python3 tests/test_core.py [-k substring] [--quick]

What is covered: sizes x segment counts, keep-alive / chunked / unknown-length /
no-Range servers, redirects, Basic auth, HTTPS (local CA, hostname + IP checks),
retries after resets, servers that push back (429 / 503, connection caps, Retry-After),
bandwidth limits, pause / resume (also while probing),
cancel cleanup, checksums, resume after SIGKILL / SIGTERM / shutdown (including
a changed remote file), duplicate detection, failure paths, concurrency, IPC
robustness (garbage, oversized frames, slow readers, client limits), disk
errors, and fd / thread leaks.
"""
import os
import re
import resource
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fluxtest import *                                              # noqa: E402,F403

QUICK = "--quick" in sys.argv
ONLY = None
if "-k" in sys.argv:
    ONLY = sys.argv[sys.argv.index("-k") + 1]

ROOT = tmpdir("fluxcore_")
OUT = ROOT / "out"
SRV = ROOT / "srv"
OUT.mkdir()
SRV.mkdir()
CORE = Core()
FILES = {}                       # name -> (size, sha256 hex)
_ids = [1000]
_lock = threading.Lock()


def nid():
    with _lock:
        _ids[0] += 1
        return _ids[0]


def mkfile(name, size):
    if name not in FILES or FILES[name][0] != size:
        FILES[name] = (size, make_file(SRV, name, size))
    return FILES[name][1]


def outpath(tag="o"):
    return OUT / f"{tag}_{nid()}.bin"


def fetch(c, site, path, *, segments=8, dest=None, timeout=90, **kw):
    """START and wait for the terminal event. Returns (id, result, dest)."""
    dl = nid()
    dest = dest or outpath()
    typ, pl = c.start(dl, site.url(path), dest, segments=segments, **kw)
    if typ != RSP_OK:
        return dl, ("start-failed", parse_error(pl) if typ == RSP_ERROR else None), dest
    return dl, c.wait_terminal(dl, timeout), dest


def completed(res):
    return bool(res) and res[0] == "complete"


def verify(label, c, dl, res, dest, name, extra=None):
    """One check: finished, right size, right SHA-256, no resume file, sane events."""
    size, sha = FILES[name][0], FILES[name][1]
    problems = []
    if not completed(res):
        problems.append(f"result={res}")
    elif not Path(dest).exists():
        problems.append("output file missing")
    else:
        if Path(dest).stat().st_size != size:
            problems.append(f"size {Path(dest).stat().st_size} != {size}")
        elif sha256_file(dest) != sha:
            problems.append("SHA-256 mismatch")
        if res[1]["total"] != size:
            problems.append(f"COMPLETE.total {res[1]['total']} != {size}")
    if Path(str(dest) + ".fluxpart").exists():
        problems.append(".fluxpart left behind")
    d = c.dl(dl)
    if d.backwards:
        problems.append(f"{d.backwards} progress frames went backwards")
    if d.over_total:
        problems.append(f"{d.over_total} progress frames exceeded the total")
    if extra:
        problems += extra
    return expect(not problems, label, "; ".join(problems))


def clean_conn(label, c):
    return expect(not c.proto_errors and c.unflagged_events == 0 and c.flagged_replies == 0,
                  label, "; ".join(c.proto_errors) +
                  f" unflagged_events={c.unflagged_events} flagged_replies={c.flagged_replies}")


def range_len(rng, size):
    m = re.match(r"bytes=(\d*)-(\d*)$", rng or "")
    if not m:
        return size
    a, b = m.group(1), m.group(2)
    if a == "":
        return int(b)
    return (int(b) if b else size - 1) - int(a) + 1


def fresh_core(**kw):
    core = Core(**kw)
    if not core.start():
        raise RuntimeError("could not start a core:\n" + core.log_text())
    return core


# ═══ Identity ═════════════════════════════════════════════════════════════════
PROJECT = Path(__file__).resolve().parent.parent


def project_version():
    """The release version written in core/include/version.h (None outside a full checkout)."""
    try:
        text = (PROJECT / "core" / "include" / "version.h").read_text()
    except OSError:
        return None
    m = re.search(r'#define\s+FLUXDM_VERSION\s+"([^"]+)"', text)
    return m.group(1) if m else None


def pom_version():
    """The daemon's own version (not its Spring Boot parent's), or None outside a full checkout."""
    import xml.etree.ElementTree as ET
    try:
        node = ET.parse(PROJECT / "daemon" / "pom.xml").getroot().find("{http://maven.apache.org/POM/4.0.0}version")
    except (OSError, ET.ParseError):
        return None
    return node.text.strip() if node is not None and node.text else None


def test_identity():
    info("Identity — one version for the core, the daemon and the wire")
    out = subprocess.run([CORE.binary, "--version"], capture_output=True, text=True, timeout=10)
    m = re.fullmatch(r"fluxdm-core (\d+\.\d+\.\d+)\n", out.stdout)
    expect(out.returncode == 0 and m is not None, f"--version prints 'fluxdm-core MAJOR.MINOR.PATCH' (got {out.stdout!r})")
    version = m.group(1) if m else None
    header, pom = project_version(), pom_version()
    if header is None or pom is None:
        note("not a full checkout (core/include/version.h or daemon/pom.xml missing): version cross-checks skipped")
    else:
        expect(version == header, f"--version ({version}) equals core/include/version.h ({header})")
        expect(version == pom, f"--version ({version}) equals the daemon's version in daemon/pom.xml ({pom})")

    site = Site(SRV)
    mkfile("identity.bin", 600 * 1024)
    c = CORE.connect()
    dl, res, dest = fetch(c, site, "/identity.bin", segments=3)
    verify("a download for the User-Agent check", c, dl, res, dest, "identity.bin")
    seen = site.requests("/identity.bin")
    agents = {r[6] for r in seen}
    expect(len(seen) >= 3, f"the probe and the segments all reached the server ({len(seen)} requests)")
    expect(agents == {f"FluxDM/{version} (+https://github.com/PhillipWanjiru/Fluxdm)"},
           "every request carries 'FluxDM/<version> (+<project URL>)' as its User-Agent", sorted(map(str, agents)))
    c.close()
    site.stop()


# ═══ Basics ═══════════════════════════════════════════════════════════════════
def test_sizes_and_segments():
    info("Sizes x segment counts (random content: a mis-placed segment cannot hide)")
    site = Site(SRV)
    c = CORE.connect()
    cases = [(1, 1), (255, 4), (4095, 4), (256 * 1024 - 1, 8), (256 * 1024, 8), ((1 << 20) + 13, 3),
             (8 << 20, 16), ((9 << 20) + 4093, 64), (3 << 20, 1), (5 << 20, 0), (2 << 20, 1000)]
    for size, segs in cases:
        name = f"sz_{size}.bin"
        mkfile(name, size)
        dl, res, dest = fetch(c, site, "/" + name, segments=segs)
        verify(f"{size:>9} bytes, segments={segs}", c, dl, res, dest, name)
    clean_conn("no protocol errors, events flagged correctly", c)
    expect(not site.requests(method="HEAD"), "the core never needs HEAD")
    c.close()
    site.stop()


def test_empty_file():
    info("Zero-byte file")
    site = Site(SRV)
    (SRV / "empty.bin").write_bytes(b"")
    FILES["empty.bin"] = (0, hashlib.sha256(b"").hexdigest())
    c = CORE.connect()
    dl, res, dest = fetch(c, site, "/empty.bin", segments=4)
    verify("empty file downloads as an empty file", c, dl, res, dest, "empty.bin")
    c.close()
    site.stop()


def test_server_flavours():
    info("Server flavours: keep-alive, chunked, unknown length, no Range support, no HEAD")
    c = CORE.connect()
    mkfile("flav.bin", 3 << 20)

    site = Site(SRV, keepalive=True)
    dl, res, dest = fetch(c, site, "/flav.bin", segments=6)
    verify("HTTP/1.1 keep-alive server, 6 segments", c, dl, res, dest, "flav.bin")
    site.stop()

    site = Site(SRV)
    site.set("/flav.bin", ranges=False)
    dl, res, dest = fetch(c, site, "/flav.bin", segments=8)
    verify("server without Range support (single stream)", c, dl, res, dest, "flav.bin")
    gets = site.requests("/flav.bin", "GET")
    expect(len(gets) == 1, f"...served with ONE request (probe connection adopted), got {len(gets)}")
    site.stop()

    site = Site(SRV)
    site.set("/flav.bin", ranges=False, chunked=True)
    dl, res, dest = fetch(c, site, "/flav.bin", segments=8)
    verify("chunked transfer (odd chunk sizes, extensions, trailer)", c, dl, res, dest, "flav.bin")
    site.stop()

    site = Site(SRV)
    site.set("/flav.bin", ranges=False, no_length=True)
    dl, res, dest = fetch(c, site, "/flav.bin", segments=8)
    verify("unknown length, body ends at EOF", c, dl, res, dest, "flav.bin")
    site.stop()

    site = Site(SRV, head="drop")
    dl, res, dest = fetch(c, site, "/flav.bin", segments=4)
    verify("server that drops HEAD requests", c, dl, res, dest, "flav.bin")
    site.stop()

    site = Site(SRV, head="405")
    dl, res, dest = fetch(c, site, "/flav.bin", segments=4)
    verify("server that answers HEAD with 405", c, dl, res, dest, "flav.bin")
    site.stop()

    site = Site(SRV, etag="", last_modified=False)
    dl, res, dest = fetch(c, site, "/flav.bin", segments=4)
    verify("server without ETag / Last-Modified", c, dl, res, dest, "flav.bin")
    site.stop()
    clean_conn("connection clean", c)
    c.close()


def test_url_forms():
    info("URL forms: percent-encoding, query strings, localhost, nested destination, long path")
    site = Site(SRV)
    c = CORE.connect()
    mkfile("with space.bin", 700 * 1024)
    dl, res, dest = fetch(c, site, "/with%20space.bin", segments=2)
    verify("percent-encoded path", c, dl, res, dest, "with space.bin")

    dl, res, dest = fetch(c, site, "/with%20space.bin?token=abc&exp=123", segments=2)
    verify("query string is preserved", c, dl, res, dest, "with space.bin")

    dl = nid()
    dest = OUT / "nested" / "a" / "b" / "c.bin"
    c.start_ok(dl, site.url("/with%20space.bin", host="localhost"), dest, segments=2)
    res = c.wait_terminal(dl, 30)
    verify("http://localhost + missing parent directories created", c, dl, res, dest, "with space.bin")

    mkfile("a/b/c/deep.bin", 300 * 1024)
    dl, res, dest = fetch(c, site, "/a/b/c/../c/deep.bin", segments=2)
    expect(completed(res) or (res and res[0] == "error"), "dot-segments in the path are handled without a crash", res)
    c.close()
    site.stop()


def test_redirects():
    info("Redirects")
    site = Site(SRV)
    c = CORE.connect()
    mkfile("redir.bin", 2 << 20)
    site.set("/hop1", redirect=(302, "/hop2"))
    site.set("/hop2", redirect=(301, f"http://127.0.0.1:{site.port}/redir.bin"))
    dl, res, dest = fetch(c, site, "/hop1", segments=4)
    verify("302 -> 301 (relative, then absolute) -> file", c, dl, res, dest, "redir.bin")
    ranged = [r for r in site.requests("/redir.bin", "GET") if r[2]]
    expect(len(ranged) >= 2, "Range headers survive the redirect chain", site.requests("/redir.bin"))

    site.set("/seven", redirect=(307, "/redir.bin"))
    dl, res, dest = fetch(c, site, "/seven", segments=4)
    verify("307", c, dl, res, dest, "redir.bin")

    site.set("/loop", redirect=(302, "/loop"))
    dl, res, dest = fetch(c, site, "/loop", segments=2, timeout=60)
    expect(res and res[0] == "error" and res[1]["code"] == ERR["NETWORK"],
           "a redirect loop ends in an error (not an endless chase)", res)
    expect(not dest.exists(), "...and leaves no output file")
    c.close()
    site.stop()


def test_basic_auth():
    info("HTTP Basic auth from URL userinfo")
    site = Site(SRV)
    c = CORE.connect()
    mkfile("auth.bin", 1 << 20)
    site.set("/auth.bin", auth=("user", "p@ss w0rd"))

    dl, res, dest = fetch(c, site, "/auth.bin", segments=4, timeout=40)
    expect(res and res[0] == "error" and res[1]["code"] == ERR["FORBIDDEN"],
           "no credentials -> FORBIDDEN", res)

    dl = nid()
    dest = outpath()
    url = site.url("/auth.bin", userinfo="user:p%40ss%20w0rd")
    c.start_ok(dl, url, dest, segments=4)
    res = c.wait_terminal(dl, 40)
    verify("credentials from the URL are sent as Basic auth", c, dl, res, dest, "auth.bin")
    log = CORE.log_text()
    expect("p%40ss" not in log and "p@ss" not in log and "w0rd" not in log,
           "the password never reaches the core's log")
    c.close()
    site.stop()


def test_checksums():
    info("Checksums")
    site = Site(SRV)
    c = CORE.connect()
    sha = mkfile("ck.bin", 2 << 20)
    dl, res, dest = fetch(c, site, "/ck.bin", segments=4, cksum_type=CK_SHA256, cksum=bytes.fromhex(sha))
    verify("SHA-256 given and correct", c, dl, res, dest, "ck.bin")
    expect(completed(res) and res[1]["checksum_ok"] == 1, "COMPLETE says checksum_ok")

    dl, res, dest = fetch(c, site, "/ck.bin", segments=4, cksum_type=CK_MD5, cksum=md5_file(SRV / "ck.bin"))
    verify("MD5 given and correct", c, dl, res, dest, "ck.bin")

    bad = bytes.fromhex(sha)[::-1]
    dl, res, dest = fetch(c, site, "/ck.bin", segments=4, cksum_type=CK_SHA256, cksum=bad)
    expect(res and res[0] == "error" and res[1]["code"] == ERR["CHECKSUM"], "wrong SHA-256 -> CHECKSUM error", res)
    expect(not dest.exists() and not Path(str(dest) + ".fluxpart").exists(),
           "...the corrupt file and its resume state are deleted")

    typ, pl = c.start(nid(), site.url("/ck.bin"), outpath(), cksum_type=9, cksum=b"x")
    expect(typ == RSP_ERROR, "unknown checksum type is rejected up front")
    c.close()
    site.stop()


# ═══ Failure paths ════════════════════════════════════════════════════════════
def test_failure_paths():
    info("Failure paths (and the original crash: a failing START among running downloads)")
    mkfile("fp_big.bin", 12 << 20)
    good = Site(SRV)
    good.set("/fp_big.bin", chunk=32768, delay=0.02)
    drop, nf = DropServer(), Site(SRV)
    refused = free_closed_port()
    c = CORE.connect()
    fd0 = CORE.proc and os.readlink(f"/proc/{CORE.pid}/fd/0")

    g1 = nid()
    gdest = outpath()
    expect(c.start_ok(g1, good.url("/fp_big.bin"), gdest, segments=4), "a good, throttled download is in flight")

    typ, pl = c.start(nid(), "not a url at all", outpath())
    expect(typ == RSP_ERROR and parse_error(pl)[0] == ERR["INVALID_URL"], "malformed URL -> immediate INVALID_URL error")
    typ, pl = c.start(nid(), "ftp://127.0.0.1/x", outpath())
    expect(typ == RSP_ERROR and parse_error(pl)[0] == ERR["INVALID_URL"], "unsupported scheme -> INVALID_URL")
    typ, pl = c.start(0, good.url("/fp_big.bin"), outpath())
    expect(typ == RSP_ERROR, "id 0 (reserved for 'global') is rejected")

    async_cases = [
        ("connection refused", f"http://127.0.0.1:{refused}/x.bin", {ERR["NETWORK"]}),
        ("DNS failure", "http://flux-nonexistent.invalid/x.bin", {ERR["NETWORK"], ERR["TIMEOUT"]}),
        ("server drops every request", f"http://127.0.0.1:{drop.port}/x.bin", {ERR["NETWORK"], ERR["TIMEOUT"]}),
        ("HTTP 404", nf.url("/missing.bin"), {ERR["NOT_FOUND"]}),
    ]
    nf.set("/forbidden.bin", status=(403, None))
    nf.set("/broken.bin", status=(500, None))
    async_cases += [("HTTP 403", nf.url("/forbidden.bin"), {ERR["FORBIDDEN"]}),
                    ("HTTP 500 (after retries)", nf.url("/broken.bin"), {ERR["SERVER"]})]
    ids = {}
    for name, url, _codes in async_cases:
        dl = nid()
        ids[name] = dl
        typ, pl = c.start(dl, url, outpath(), segments=4)
        expect(typ == RSP_OK, f"{name}: START acknowledged first", parse_error(pl) if typ == RSP_ERROR else None)
    for name, url, codes in async_cases:
        res = c.wait_terminal(ids[name], 90)
        ok_ = res is not None and res[0] == "error" and res[1]["code"] in codes
        expect(ok_, f"{name}: async error event with code in {sorted(codes)}", res)
        d = c.dl(ids[name])
        expect(d.first_reply_idx is not None and d.first_event_idx is not None and
               d.first_reply_idx < d.first_event_idx, f"{name}: OK reply precedes the error event")
        st = c.status(ids[name])
        expect(isinstance(st, dict) and st["status"] == ST["ERROR"], f"{name}: STATUS reports ERROR", st)
        expect(c.cancel(ids[name]) is True, f"{name}: CANCEL clears it")
        expect(isinstance(c.status(ids[name]), tuple), f"{name}: ...and it is gone afterwards")
        expect(CORE.alive(), f"core alive after {name}")

    res = c.wait_terminal(g1, 120)
    verify("the good download finished untouched by all of that", c, g1, res, gdest, "fp_big.bin")
    expect(os.readlink(f"/proc/{CORE.pid}/fd/0") == fd0, "stdin (fd 0) was never closed by the core")
    clean_conn("no corrupted frames", c)
    drop.stop()
    nf.stop()
    good.stop()
    c.close()


def test_retry_policies():
    info("Retries: connection resets mid-body, 503 + Retry-After")
    c = CORE.connect()
    size = 6 << 20
    mkfile("rst.bin", size)
    site = Site(SRV)
    site.set("/rst.bin", reset_after=300_000, reset_times=6, chunk=16384)
    dl, res, dest = fetch(c, site, "/rst.bin", segments=4, timeout=120)
    verify("download survives 6 mid-body resets", c, dl, res, dest, "rst.bin")
    resets = [r for r in site.requests("/rst.bin") if "reset" in str(r[3])]
    expect(len(resets) == 6, f"...the server really did reset 6 times (saw {len(resets)})")
    site.stop()

    site = Site(SRV)
    site.set("/rst.bin", status=(503, 1), status_times=3)
    t0 = time.time()
    dl, res, dest = fetch(c, site, "/rst.bin", segments=4, timeout=120)
    verify("503 with Retry-After is retried and then succeeds", c, dl, res, dest, "rst.bin")
    expect(time.time() - t0 < 30, f"...without waiting unreasonably long ({time.time() - t0:.1f}s)")
    site.stop()
    c.close()


# ═══ Servers that push back ═══════════════════════════════════════════════════
def _refusals(site, path):
    return [r for r in site.requests(path, "GET") if r[3] in (429, 503)]


def test_rate_limits():
    info("Rate limits: HTTP 429 / 503 are waited out instead of failing the download")
    c = CORE.connect()
    mkfile("rl_cap.bin", 16 << 20)
    mkfile("rl_slow.bin", 12 << 20)

    # 1. A connection cap. The server serves 3 GETs at a time and answers the rest with 429, the
    #    way Hetzner's speed-test servers did to 24 parallel segments. The gate learns the cap from
    #    the refusals; the other segments wait for a slot instead of burning their retries.
    #    The server is slow on purpose (~400 KB/s per connection, so a segment takes ~4 s): that is
    #    what separates "wait for a slot" from "retry on a timer" — a segment that merely backs off
    #    (1 s, 2 s, 4 s ...) runs into the full server again and again, and is refused 15+ times.
    site = Site(SRV, max_active=3, chunk=16384, delay=0.04)
    t0 = time.time()
    dl, res, dest = fetch(c, site, "/rl_slow.bin", segments=8, timeout=120)
    dt = time.time() - t0
    verify("a server that takes 3 connections at a time still delivers 8 segments intact", c, dl, res, dest, "rl_slow.bin")
    expect(site.peak_active <= 3, f"...it never saw more than its cap in flight ({site.peak_active})")
    expect(1 <= site.refused <= 9, f"...with few refusals: segments wait for a slot, they do not hammer ({site.refused})")
    expect(dt < 60, f"...in a sane time ({dt:.1f}s)")
    site.stop()

    # 2. 503 is the same story (nginx's limit_conn answers 503), and Retry-After is honoured.
    site = Site(SRV, max_active=2, limit_status=(503, 1), chunk=32768, delay=0.01)
    dl, res, dest = fetch(c, site, "/rl_cap.bin", segments=6, timeout=120)
    verify("503 + Retry-After: 1 from a 2-connection server: the download still completes", c, dl, res, dest, "rl_cap.bin")
    expect(site.refused >= 1, f"...and the server really did push back ({site.refused} refusals)")
    site.stop()

    # 3. Retry-After holds the whole host: after the first refusal no new request arrives until it is over.
    site = Site(SRV, max_active=2, limit_status=(429, 2), chunk=32768, delay=0.01)
    dl, res, dest = fetch(c, site, "/rl_cap.bin", segments=6, timeout=120)
    verify("429 + Retry-After: 2: completes intact", c, dl, res, dest, "rl_cap.bin")
    said = [l for l in CORE.log_text().splitlines() if f"127.0.0.1:{site.port} is refusing" in l]
    expect(len(said) == 1 and "asks us to wait 2 s" in said[0],
           "...and the log says at once that the server asked us to wait 2 s", "; ".join(said[:3]))
    gets = site.requests("/rl_cap.bin", "GET")
    refused_at = [r[5] for r in gets if r[3] == 429]
    if expect(bool(refused_at), "...the server did refuse some requests"):
        t_ref = min(refused_at)
        later = sorted(r[5] for r in gets if r[5] > t_ref + 0.5)
        expect(bool(later) and later[0] - t_ref >= 1.9,
               "...and nobody asked again before the Retry-After had passed "
               f"(next request {later[0] - t_ref:.2f}s after the first refusal)" if later else "...no later request?")
    site.stop()

    # 4. Several downloads from one capped server share what it allows.
    mkfile("rl_a.bin", 6 << 20)
    site = Site(SRV, max_active=3, chunk=32768, delay=0.01)
    jobs = []
    for _ in range(3):
        dl, dest = nid(), outpath()
        expect(c.start_ok(dl, site.url("/rl_a.bin"), dest, segments=4), "a download is started")
        jobs.append((dl, dest))
    for n, (dl, dest) in enumerate(jobs, 1):
        verify(f"3 downloads x 4 segments from a 3-connection server: #{n} is intact", c, dl,
               c.wait_terminal(dl, 120), dest, "rl_a.bin")
    expect(site.peak_active <= 3 and site.refused <= 40,
           f"...peak {site.peak_active} in flight, {site.refused} refusals in all")
    site.stop()

    # 5. A server that relents after a few refusals.
    site = Site(SRV)
    site.set("/rl_a.bin", status=(429, None), status_after=1, status_times=6)
    t0 = time.time()
    dl, res, dest = fetch(c, site, "/rl_a.bin", segments=4, timeout=120)
    verify("the first 6 requests after the probe are refused, then the server relents: completes intact", c, dl, res, dest, "rl_a.bin")
    expect(time.time() - t0 < 40, f"...in a sane time ({time.time() - t0:.1f}s)")
    site.stop()

    # 6. Retry-After is about the host, not about one segment: a second download that starts while
    #    the pause is running keeps off the server until it is over (no request, not even a probe).
    mkfile("rl_b.bin", 2 << 20)
    site = Site(SRV, max_active=2, limit_status=(429, 3), chunk=16384, delay=0.04)
    dl_a, dest_a = nid(), outpath()
    expect(c.start_ok(dl_a, site.url("/rl_b.bin"), dest_a, segments=4), "download A is started")
    t_end = time.time() + 15
    while not _refusals(site, "/rl_b.bin") and time.time() < t_end:
        time.sleep(0.02)
    first = _refusals(site, "/rl_b.bin")
    if expect(bool(first), "...and the 2-connection server pushes back (429, Retry-After: 3)"):
        t_ref = min(r[5] for r in first)
        time.sleep(max(0.0, t_ref + 1.0 - time.monotonic()))
        dl_b, dest_b = nid(), outpath()
        expect(c.start_ok(dl_b, site.url("/rl_b.bin"), dest_b, segments=2), "download B is started one second into the pause")
        for name, d, dest in (("A", dl_a, dest_a), ("B", dl_b, dest_b)):
            verify(f"download {name} completes intact", c, d, c.wait_terminal(d, 120), dest, "rl_b.bin")
        later = sorted(r[5] for r in site.requests("/rl_b.bin", "GET") if r[5] > t_ref + 0.5)
        expect(bool(later) and later[0] - t_ref >= 2.9,
               "...and nothing reached the server while the host was paused "
               f"(next request {later[0] - t_ref:.2f}s after the first refusal)" if later else "...no later request?")
    else:
        c.cancel(dl_a)
    site.stop()

    # 7. Control stays prompt while every segment is waiting out a long Retry-After.
    site = Site(SRV)
    site.set("/rl_a.bin", status=(429, 30), status_after=1)
    dl, dest = nid(), outpath()
    expect(c.start_ok(dl, site.url("/rl_a.bin"), dest, segments=4), "START acknowledged")
    time.sleep(1.5)
    t0 = time.time()
    r = c.pause(dl)
    expect(r is True and time.time() - t0 < 4, f"PAUSE while all segments wait out Retry-After: 30 is prompt ({time.time() - t0:.1f}s)", r)
    st = c.status(dl)
    expect(isinstance(st, dict) and st["status"] == ST["PAUSED"], "...and the download is PAUSED", st)
    expect(c.cancel(dl) is True, "CANCEL cleans up")
    expect(not dest.exists() and not Path(str(dest) + ".fluxpart").exists(), "...leaving no files behind")
    expect(CORE.alive(), "core alive")
    site.stop()
    clean_conn("connection clean", c)
    c.close()


def test_rate_limit_patience():
    info("Rate limits: how long a download waits for a server that keeps refusing (--patience)")
    mkfile("rl_never.bin", 4 << 20)

    # The core's own switch: bad values are refused up front.
    core = Core()
    for bad in ("abc", "-1", "86401", ""):
        r = subprocess.run([core.binary, "--patience", bad], capture_output=True, timeout=10)
        expect(r.returncode == 2 and b"patience" in r.stderr, f"--patience {bad!r} is rejected (exit code {r.returncode})")
    shutil.rmtree(core.dir, ignore_errors=True)

    core = fresh_core(args=["--patience", "3"])
    c = core.connect()
    try:
        # 1. every segment refused, the probe let through: gives up after the patience, not after N retries
        site = Site(SRV)
        site.set("/rl_never.bin", status=(429, None), status_after=1)
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/rl_never.bin", segments=4, timeout=60)
        dt = time.time() - t0
        ok_ = bool(res) and res[0] == "error" and res[1]["code"] == ERR["SERVER"]
        expect(ok_, "a server that never stops refusing: the download fails with a SERVER error", res)
        msg = res[1]["msg"] if ok_ else ""
        expect("429" in msg and "kept refusing" in msg and "gave up after" not in msg, f"...and says why ({msg!r})")
        expect(2.5 <= dt <= 15, f"...after about the patience, not after a retry count ({dt:.1f}s)")
        n = len(_refusals(site, "/rl_never.bin"))
        expect(n <= 14, f"...having asked a bounded number of times ({n} refusals for 4 segments)")
        expect(c.cancel(dl) is True and not dest.exists(), "...CANCEL clears it and no file is left")
        site.stop()

        # 2. the probe itself is refused
        site = Site(SRV)
        site.set("/rl_never.bin", status=(503, None))
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/rl_never.bin", segments=4, timeout=60)
        dt = time.time() - t0
        ok_ = bool(res) and res[0] == "error" and res[1]["code"] == ERR["SERVER"]
        expect(ok_ and "503" in res[1]["msg"] and "kept refusing" in res[1]["msg"],
               "a server that refuses even the probe: SERVER error that says so", res)
        expect(2.5 <= dt <= 12, f"...after about the patience ({dt:.1f}s)")
        n = len(site.requests("/rl_never.bin"))
        expect(2 <= n <= 5, f"...with a few polite attempts, not a hammering ({n} requests)")
        c.cancel(dl)
        site.stop()

        # 3. a Retry-After beyond the patience is not waited for
        for label, rule in (("the probe", dict(status=(429, 120))),
                            ("a segment", dict(status=(429, 120), status_after=1))):
            site = Site(SRV)
            site.set("/rl_never.bin", **rule)
            t0 = time.time()
            dl, res, dest = fetch(c, site, "/rl_never.bin", segments=4, timeout=60)
            dt = time.time() - t0
            ok_ = bool(res) and res[0] == "error" and res[1]["code"] == ERR["SERVER"]
            expect(ok_, f"Retry-After: 120 on {label}, patience 3 s: fails at once instead of waiting", res)
            expect(dt < 3, f"...promptly ({dt:.1f}s)")
            if ok_ and label == "a segment":
                expect("asks us to wait 120 s" in res[1]["msg"], f"...and tells the user what the server asked ({res[1]['msg']!r})")
            c.cancel(dl)
            site.stop()
        clean_conn("connection clean", c)
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)

    # The patience is counted per run of refusals: once the server serves us again the clock starts
    # over. Two runs of ~3 s each, with a response between them, are not 6 s of refusing.
    # (One segment, so the requests arrive in a known order; the script is indexed by request:
    #  #0 probe, #1 and #2 refused, #3 answered but reset after 1 KiB, #4 and #5 refused, then normal.)
    core = fresh_core(args=["--patience", "4"])
    c = core.connect()
    try:
        site = Site(SRV)
        site.set("/rl_never.bin", script=[None, (429, None), (429, None), ("reset", 1024), (429, None), (429, None)])
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/rl_never.bin", segments=1, timeout=60)
        dt = time.time() - t0
        verify("two runs of refusals with a response between them: each gets its own patience (4 s)", c, dl, res, dest, "rl_never.bin")
        expect(len(_refusals(site, "/rl_never.bin")) == 4, f"...the server refused 4 times in all ({len(_refusals(site, '/rl_never.bin'))})")
        expect(dt > 5.0, f"...so the whole wait was longer than the patience ({dt:.1f}s)")
        site.stop()
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)

    # --patience 0: the first refusal ends it
    core = fresh_core(args=["--patience", "0"])
    c = core.connect()
    try:
        site = Site(SRV)
        site.set("/rl_never.bin", status=(429, None))
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/rl_never.bin", segments=4, timeout=30)
        expect(bool(res) and res[0] == "error" and time.time() - t0 < 3 and len(site.requests("/rl_never.bin")) == 1,
               f"--patience 0: the first refusal is final (one request, {time.time() - t0:.1f}s)", res)
        c.cancel(dl)
        site.stop()
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)


def test_rate_limit_queue():
    info("Rate limits: queuing behind a server's limit is not being ignored")
    mkfile("rl_q.bin", 3 << 20)
    mkfile("rl_b.bin", 2 << 20)
    mkfile("rl_a.bin", 6 << 20)

    core = fresh_core(args=["--patience", "2"])
    c = core.connect()
    try:
        # 1. A segment refused once, then queued behind the other segment of its own download, must not
        #    run out of patience because that first refusal is old: the host never went silent, it was
        #    feeding the other segment the whole time. 2 segments of 1.5 MiB at ~400 KB/s: one streams
        #    for ~4 s — twice the patience of 2 s — while the other is turned away, waits for a slot
        #    (the gate has learnt the server tolerates one) and, when its turn comes, is turned away
        #    once more. Its first refusal is ancient history by then.
        #    (server-side request order: #0 probe, #1 the segment that gets in, #2 the one refused,
        #     #3 its retry after the first has finished: refused again, then the server relents)
        site = Site(SRV, chunk=16384, delay=0.04)
        site.set("/rl_q.bin", script=[None, None, (429, None), (429, None)])
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/rl_q.bin", segments=2, timeout=120)
        dt = time.time() - t0
        verify("a segment refused, queued for 4 s and refused again, patience 2 s: the host was feeding "
               "the other segment all along, so the download completes", c, dl, res, dest, "rl_q.bin")
        refs = sorted(r[5] for r in _refusals(site, "/rl_q.bin"))
        expect(len(refs) == 2 and refs[-1] - refs[0] > 2.5,
               "...the scenario was real: the second refusal came well after the patience had run out "
               f"since the first ({refs[-1] - refs[0]:.1f}s apart)" if len(refs) >= 2 else f"...{len(refs)} refusal(s)?")
        expect(dt > 6, f"...and the two segments were served one after the other ({dt:.1f}s)")
        site.stop()

        # 2. The same for a download that has only just begun: its probe is turned away because the
        #    server's one connection is busy feeding another download, and it keeps asking while that lasts.
        site = Site(SRV, max_active=1, chunk=16384, delay=0.04)
        dl_a, dest_a = nid(), outpath()
        expect(c.start_ok(dl_a, site.url("/rl_b.bin"), dest_a, segments=1), "download A starts (1 segment, ~5 s)")
        time.sleep(1.0)
        dl_b, dest_b = nid(), outpath()
        expect(c.start_ok(dl_b, site.url("/rl_b.bin"), dest_b, segments=1), "download B starts while A is being served")
        for name, d, dest in (("A", dl_a, dest_a), ("B", dl_b, dest_b)):
            verify(f"download {name} completes intact (B waited ~4 s for the one connection, patience 2 s)",
                   c, d, c.wait_terminal(d, 120), dest, "rl_b.bin")
        expect(len(_refusals(site, "/rl_b.bin")) >= 2, f"...B's probe was refused repeatedly ({len(_refusals(site, '/rl_b.bin'))} refusals)")
        site.stop()
        clean_conn("connection clean", c)
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)

    # 3. The log. 3 downloads x 8 segments against a 2-connection server: 20-odd segments are turned
    #    away within a second. The user should read that the host started refusing, and where its
    #    allowance settled — once each, not one line for every refused segment.
    core = fresh_core()
    c = core.connect()
    try:
        site = Site(SRV, max_active=2, chunk=32768, delay=0.02)
        jobs = []
        for _ in range(3):
            dl, dest = nid(), outpath()
            expect(c.start_ok(dl, site.url("/rl_a.bin"), dest, segments=8), "a download of 8 segments starts")
            jobs.append((dl, dest))
        for n, (dl, dest) in enumerate(jobs, 1):
            verify(f"3 downloads x 8 segments through a 2-connection server: #{n} is intact", c, dl,
                   c.wait_terminal(dl, 120), dest, "rl_a.bin")
        expect(site.refused >= 8, f"...the wave was real ({site.refused} refusals)")
        log = core.log_text().splitlines()
        started = [l for l in log if "is refusing connections" in l]
        settled = [l for l in log if "using at most" in l]
        expect(len(started) == 1 and "HTTP 429" in started[0],
               f"...yet the log says the host started refusing exactly once ({len(started)} line(s))", "; ".join(started[:3]))
        expect(1 <= len(settled) <= 2 and any(re.search(r"at most [12] connections? at a time", l) for l in settled),
               f"...and where its allowance settled, once or twice ({len(settled)} line(s))", "; ".join(settled[:3]))
        site.stop()
        clean_conn("connection clean", c)
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)

    # 4. A server that ignores Range. The probe's connection is kept as the download's one connection
    #    and counts against the host like any other. Two such downloads are being served (their bodies
    #    are held open) when a third is turned away: the host evidently takes two, and the log must say
    #    so — not one, as if the core had lost track of its own connections.
    core = fresh_core()
    c = core.connect()
    try:
        site = Site(SRV, ranges=False, max_active=2, hold_after=65536, chunk=16384)
        jobs = []
        for _ in range(2):
            dl, dest = nid(), outpath()
            expect(c.start_ok(dl, site.url("/rl_b.bin"), dest, segments=4), "a download from a server that ignores Range starts")
            jobs.append((dl, dest))

        def streaming():
            return all((c.dl(d).progress or {}).get("done", 0) > 0 for d, _ in jobs)

        t_end = time.time() + 15
        while time.time() < t_end and not streaming():
            time.sleep(0.02)
        expect(streaming() and site.active_gets == 2,
               f"...both are being served, bodies held, by the 2-connection server ({site.active_gets} active)")
        dl, dest = nid(), outpath()
        expect(c.start_ok(dl, site.url("/rl_b.bin"), dest, segments=4), "a third download starts and is turned away")
        jobs.append((dl, dest))
        t_end = time.time() + 20
        while time.time() < t_end and "using at most" not in core.log_text():
            time.sleep(0.05)
        site.release.set()
        for n, (dl, dest) in enumerate(jobs, 1):
            verify(f"download #{n} completes intact once the held bodies are let go", c, dl,
                   c.wait_terminal(dl, 60), dest, "rl_b.bin")
        settled = [l for l in core.log_text().splitlines() if "using at most" in l]
        expect(bool(settled) and all("at most 2 connections at a time" in l for l in settled),
               "...and the log says the host takes 2 connections: the two it was serving were counted",
               "; ".join(settled[:3]) or "no 'using at most' line")
        site.stop()
        clean_conn("connection clean", c)
    finally:
        c.close()
        expect(core.stop() == 0, "core exits cleanly")
        shutil.rmtree(core.dir, ignore_errors=True)


def test_disk_errors():
    info("Disk errors")
    site = Site(SRV)
    c = CORE.connect()
    mkfile("disk.bin", 1 << 20)
    (ROOT / "afile").write_text("x")

    for label, dest in (("destination below a regular file", ROOT / "afile" / "sub" / "o.bin"),
                        ("destination below /proc", Path("/proc/fluxdm-nope/o.bin"))):
        dl = nid()
        typ, pl = c.start(dl, site.url("/disk.bin"), dest, segments=2)
        if typ == RSP_ERROR:
            ok_ = parse_error(pl)[0] == ERR["DISK"]
            expect(ok_, f"{label}: immediate DISK error", parse_error(pl))
        else:
            res = c.wait_terminal(dl, 30)
            expect(res and res[0] == "error" and res[1]["code"] == ERR["DISK"], f"{label}: DISK error event", res)
    expect((ROOT / "afile").read_text() == "x", "an unrelated file in the way is left untouched")

    # RLIMIT_FSIZE with SIGXFSZ ignored: writes fail with EFBIG, which is as good as "disk full".
    def limits():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (300 * 1024, 300 * 1024))
    core = Core()
    core.log_level = 1
    core.start_fn = None
    try:
        core.log = open(core.log_path, "ab")
        core.proc = subprocess.Popen([core.binary, "--sock", core.sock_path, "--log", "1"], stdout=core.log,
                                     stderr=core.log, preexec_fn=limits)
        for _ in range(100):
            if os.path.exists(core.sock_path):
                break
            time.sleep(0.05)
        c2 = core.connect()
        dl = nid()
        dest = outpath("full")
        typ, pl = c2.start(dl, site.url("/disk.bin"), dest, segments=2)
        if typ == RSP_ERROR:
            expect(parse_error(pl)[0] == ERR["DISK"], "file-size limit: immediate DISK error", parse_error(pl))
        else:
            res = c2.wait_terminal(dl, 40)
            expect(res and res[0] == "error" and res[1]["code"] == ERR["DISK"],
                   "file-size limit (EFBIG while writing): DISK error event", res)
        expect(core.alive(), "core survives failing writes")
        c2.close()
    finally:
        core.stop()
    c.close()
    site.stop()


# ═══ Control: pause / resume / cancel / limits ════════════════════════════════
def test_pause_resume():
    info("Pause / resume (deterministic: the server stalls every connection until released)")
    size = 8 << 20
    mkfile("pr.bin", size)
    site = Site(SRV)
    site.hold("/pr.bin", after=256 * 1024, chunk=32768)
    c = CORE.connect()
    dl, dest = nid(), outpath()
    expect(c.start_ok(dl, site.url("/pr.bin"), dest, segments=4), "START acknowledged")
    expect(c.wait_done(dl, 512 * 1024, 20), "download is in flight (server holds it)")
    t0 = time.time()
    expect(c.pause(dl) is True, f"PAUSE acknowledged ({time.time() - t0:.2f}s)")
    st = c.wait_status(dl, ST["PAUSED"], 5)
    expect(isinstance(st, dict) and st["status"] == ST["PAUSED"], "status is PAUSED", st)
    expect(Path(str(dest) + ".fluxpart").exists(), "resume state is on disk while paused")
    done1 = st["done"] if isinstance(st, dict) else -1
    site.release.set()
    time.sleep(1.2)
    st2 = c.status(dl)
    expect(isinstance(st2, dict) and st2["done"] == done1, "no bytes arrive while paused", (done1, st2))
    expect(c.pause(dl) is True, "PAUSE of a paused download is a no-op")
    before = site.log_len()
    expect(c.resume(dl) is True, "RESUME acknowledged")
    res = c.wait_terminal(dl, 60)
    verify("resumed download completes with the right bytes", c, dl, res, dest, "pr.bin")
    resent = site.sent_bytes("/pr.bin", since=before)
    expect(resent <= size - done1 + 256 * 1024,
           f"resumed from the partial file: {resent} bytes fetched after RESUME, {size - done1} still missing")
    r = c.pause(dl)
    expect(isinstance(r, tuple), "PAUSE of a completed download is an error", r)
    r = c.resume(9999999)
    expect(isinstance(r, tuple) and r[0] == ERR["NOT_FOUND"], "RESUME of an unknown id -> NOT_FOUND", r)
    c.close()
    site.stop()


def test_pause_resume_storm():
    info("Pause / resume storm on a throttled download")
    size = 16 << 20
    mkfile("storm.bin", size)
    site = Site(SRV)
    site.set("/storm.bin", chunk=16384, delay=0.05)          # ~2 MB/s in total: about 8 s
    c = CORE.connect()
    dl, dest = nid(), outpath()
    c.start_ok(dl, site.url("/storm.bin"), dest, segments=6)
    c.wait_done(dl, 256 * 1024, 20)
    bad, cycles = [], 0
    for i in range(12):
        st = c.status(dl)
        if isinstance(st, dict) and st["status"] == ST["COMPLETE"]:
            break                                              # too fast for more cycles
        if c.pause(dl) is not True:
            bad.append(f"pause {i}")
        st = c.wait_status(dl, ST["PAUSED"], 5)
        if not (isinstance(st, dict) and st["status"] == ST["PAUSED"]):
            bad.append(f"paused state {i}: {st}")
        time.sleep(0.05 * (i % 4))
        if c.resume(dl) is not True:
            bad.append(f"resume {i}")
        time.sleep(0.1 + 0.05 * (i % 3))
        cycles += 1
    expect(not bad and cycles >= 8, f"{cycles} pause/resume cycles all acknowledged and reflected in STATUS", bad)
    res = c.wait_terminal(dl, 120)
    verify("download still finishes correctly after the storm", c, dl, res, dest, "storm.bin")
    expect(CORE.alive(), "core alive")
    c.close()
    site.stop()


def test_cancel_cleanup():
    info("Cancel")
    size = 8 << 20
    mkfile("cc.bin", size)
    site = Site(SRV)
    site.hold("/cc.bin", after=128 * 1024)
    c = CORE.connect()

    dl, dest = nid(), outpath()
    c.start_ok(dl, site.url("/cc.bin"), dest, segments=4)
    c.wait_done(dl, 256 * 1024, 20)
    t0 = time.time()
    expect(c.cancel(dl) is True, f"CANCEL while downloading ({time.time() - t0:.2f}s)")
    time.sleep(0.3)
    expect(not dest.exists() and not Path(str(dest) + ".fluxpart").exists(), "partial data and resume file removed")
    expect(isinstance(c.status(dl), tuple), "download is forgotten")
    expect(c.cancel(dl) is True, "CANCEL again is idempotent")

    dl, dest = nid(), outpath()
    c.start_ok(dl, site.url("/cc.bin"), dest, segments=4)
    c.wait_done(dl, 256 * 1024, 20)
    c.pause(dl)
    c.wait_status(dl, ST["PAUSED"], 5)
    expect(c.cancel(dl) is True, "CANCEL while paused")
    expect(not dest.exists() and not Path(str(dest) + ".fluxpart").exists(), "paused download's files removed too")

    site.release.set()
    site.set("/cc.bin")
    dl, res, dest = fetch(c, site, "/cc.bin", segments=4)
    verify("(a completed download)", c, dl, res, dest, "cc.bin")
    expect(c.cancel(dl) is True, "CANCEL of a COMPLETED download")
    expect(dest.exists() and sha256_file(dest) == FILES["cc.bin"][1], "...never deletes the finished file")
    c.close()
    site.stop()


def test_interruptible_probe():
    info("Control while the server is silent (probe must be interruptible)")
    silent = SilentServer()
    c = CORE.connect()
    dl, dest = nid(), outpath()
    c.start_ok(dl, f"http://127.0.0.1:{silent.port}/x.bin", dest)
    time.sleep(0.5)
    t0 = time.time()
    r = c.pause(dl)
    dt = time.time() - t0
    expect(r is True and dt < 3, f"PAUSE during the probe returns promptly ({dt:.2f}s)", r)
    st = c.wait_status(dl, ST["PAUSED"], 5)
    expect(isinstance(st, dict) and st["status"] == ST["PAUSED"], "probing download is PAUSED", st)
    expect(c.resume(dl) is True, "RESUME relaunches the probe")
    time.sleep(0.5)
    t0 = time.time()
    r = c.cancel(dl)
    dt = time.time() - t0
    expect(r is True and dt < 3, f"CANCEL during the probe returns promptly ({dt:.2f}s)", r)
    expect(isinstance(c.status(dl), tuple), "download is gone")
    c.close()
    silent.stop()


def test_bandwidth_limits():
    info("Bandwidth limits")
    c = CORE.connect()
    size = 3 << 20
    mkfile("bw.bin", size)
    site = Site(SRV)

    t0 = time.time()
    dl, res, dest = fetch(c, site, "/bw.bin", segments=4, bw_limit=1 << 20)
    dt = time.time() - t0
    verify("per-download limit 1 MiB/s on 3 MiB", c, dl, res, dest, "bw.bin")
    expect(2.0 <= dt <= 8, f"...took {dt:.1f}s (expected about 2.75 s)")

    expect(c.set_limit(0, 1 << 20) is True, "global limit 1 MiB/s set")
    t0 = time.time()
    dl, res, dest = fetch(c, site, "/bw.bin", segments=4)
    dt = time.time() - t0
    verify("global limit applies to a download that sets none", c, dl, res, dest, "bw.bin")
    expect(2.0 <= dt <= 8, f"...took {dt:.1f}s")
    expect(c.set_limit(0, 0) is True, "global limit lifted")

    # The throttle must restart when the limit is raised mid-flight.
    size2 = 6 << 20
    mkfile("bw2.bin", size2)
    dl, dest = nid(), outpath()
    c.start_ok(dl, site.url("/bw2.bin"), dest, segments=4, bw_limit=256 * 1024)
    time.sleep(1.2)
    st = c.status(dl)
    expect(isinstance(st, dict) and st["done"] < 2 << 20, "download is being held back (256 KiB/s)", st)
    expect(c.set_limit(dl, 0) is True, "per-download limit lifted mid-flight")
    t0 = time.time()
    res = c.wait_terminal(dl, 30)
    verify(f"finishes quickly once unthrottled ({time.time() - t0:.1f}s)", c, dl, res, dest, "bw2.bin")
    expect(time.time() - t0 < 10, "...throttled connections were restarted")
    c.close()
    site.stop()


def test_duplicates_and_ids():
    info("Duplicate detection, id handling")
    size = 4 << 20
    mkfile("dup.bin", size)
    site = Site(SRV)
    site.hold("/dup.bin", after=128 * 1024)
    c = CORE.connect()
    dl, dest = nid(), outpath()
    c.start_ok(dl, site.url("/dup.bin"), dest, segments=2)
    c.wait_done(dl, 128 * 1024, 20)
    typ, pl = c.start(dl, site.url("/dup.bin"), outpath())
    expect(typ == RSP_ERROR, "START with the id of a live download is rejected")
    typ, pl = c.start(nid(), site.url("/dup.bin"), dest)
    expect(typ == RSP_ERROR, "START onto the destination of a live download is rejected")
    expect(c.cancel(dl) is True, "cleanup")
    site.release.set()
    site.set("/dup.bin")

    dl, res, dest = fetch(c, site, "/dup.bin", segments=2)
    verify("first run of a recycled id", c, dl, res, dest, "dup.bin")
    typ, _ = c.start(dl, site.url("/dup.bin"), dest, segments=2)
    expect(typ == RSP_OK, "a finished download's id can be started again")
    res = c.wait_terminal(dl, 30)
    verify("...and it completes again", c, dl, res, dest, "dup.bin")

    for big in (5000, 70000, 4294967295):
        dest = outpath()
        c.start_ok(big, site.url("/dup.bin"), dest, segments=2)
        res = c.wait_terminal(big, 30)
        verify(f"id {big}", c, big, res, dest, "dup.bin")
    c.close()
    site.stop()


# ═══ Persistence: shutdown, crash, resume ═════════════════════════════════════
def _resume_scenario(label, how, change_remote=False, url_query=None):
    """Start a held download, stop the core by `how` ('shutdown' | 'sigterm' | 'sigkill'),
    restart it on the same socket and finish the download."""
    size = 12 << 20
    name = f"rs_{label}.bin"
    mkfile(name, size)
    site = Site(SRV)
    path = "/" + name
    site.hold(path, after=1 << 20)
    core = fresh_core()
    c = core.connect()
    dl, dest = nid(), outpath("resume")
    url = site.url(path) + (f"?{url_query[0]}" if url_query else "")
    c.start_ok(dl, url, dest, segments=4)
    expect(c.wait_done(dl, 3 << 20, 30), f"[{label}] 4 MiB received while the server holds the rest")
    done_at_stop = c.dl(dl).max_done
    if how == "sigkill":
        time.sleep(4.0)                                  # let the periodic checkpoint run
        core.kill()
        rc = None
    else:
        t0 = time.time()
        rc = core.stop(via="command" if how == "shutdown" else "signal")
        expect(rc == 0 and time.time() - t0 < 5, f"[{label}] core exits cleanly on {how} ({time.time() - t0:.1f}s, rc={rc})")
    fp = Path(str(dest) + ".fluxpart")
    expect(fp.exists(), f"[{label}] resume file survives")
    expect(dest.exists(), f"[{label}] partial data file survives")

    if change_remote:
        p = SRV / name
        FILES[name] = (size, make_file(SRV, name, size))        # new random content, new mtime
        os.utime(p, (time.time() + 600, time.time() + 600))
    site.release.set()
    site.set(path)
    before = site.log_len()
    expect(core.start(clean=(how != "sigkill")), f"[{label}] core restarts" +
           (" over the stale socket file" if how == "sigkill" else ""), core.log_text())
    c2 = core.connect()
    if url_query:
        url = site.url(path) + f"?{url_query[1]}"
    typ, pl = c2.start(dl, url, dest, segments=4)
    expect(typ == RSP_OK, f"[{label}] same download started again", parse_error(pl) if typ == RSP_ERROR else None)
    res = c2.wait_terminal(dl, 90)
    verify(f"[{label}] final file is byte-exact", c2, dl, res, dest, name)
    fetched = site.sent_bytes(path, since=before)
    if change_remote:
        expect(fetched >= size - 64 * 1024, f"[{label}] changed remote -> stale data discarded, re-fetched ({fetched} bytes)")
    else:
        expect(fetched <= size - done_at_stop + 256 * 1024,
               f"[{label}] resumed: fetched {fetched} bytes, {size - done_at_stop} were missing")
    expect(core.stop() == 0, f"[{label}] second core exits cleanly")
    shutil.rmtree(core.dir, ignore_errors=True)
    site.stop()


def test_resume_after_shutdown():
    info("Resume: after CMD_SHUTDOWN")
    _resume_scenario("shutdown", "shutdown")


def test_resume_after_sigterm():
    info("Resume: after SIGTERM")
    _resume_scenario("sigterm", "sigterm")


def test_resume_after_sigkill():
    info("Resume: after SIGKILL (crash), over the stale socket file")
    _resume_scenario("sigkill", "sigkill")


def test_resume_validation():
    info("Resume validation")
    _resume_scenario("changed", "shutdown", change_remote=True)
    _resume_scenario("token", "shutdown", url_query=("token=1", "token=2"))


def test_second_instance():
    info("A second core on the same socket / a non-socket in the way")
    c = CORE.connect()
    p = subprocess.run([CORE.binary, "--sock", CORE.sock_path, "--log", "0"], capture_output=True, timeout=10)
    expect(p.returncode != 0, "a second core refuses to take over a live socket", p.stderr.decode())
    expect(isinstance(c.status(424242), tuple), "...and the first core still answers")
    c.close()
    victim = ROOT / "notasocket"
    victim.write_text("precious")
    p = subprocess.run([CORE.binary, "--sock", str(victim), "--log", "0"], capture_output=True, timeout=10)
    expect(p.returncode != 0, "a core refuses to bind over a regular file")
    expect(victim.read_text() == "precious", "...and does not delete it")


# ═══ Concurrency and IPC robustness ═══════════════════════════════════════════
def test_concurrent_downloads():
    info("Concurrency: many downloads (good and failing) started at once, from several threads")
    site = Site(SRV)
    drop, nf = DropServer(), Site(SRV)
    refused = free_closed_port()
    n_good = 10 if not QUICK else 5
    sizes = [(1 << 20) + i * 311_111 for i in range(n_good)]
    names = []
    for i, sz in enumerate(sizes):
        nm = f"cc_{i}.bin"
        mkfile(nm, sz)
        site.set("/" + nm, chunk=32768, delay=0.004)
        names.append(nm)
    nf.set("/forbidden", status=(403, None))
    fd_before, thr_before = CORE.fd_count(), CORE.thread_count()
    c = CORE.connect()
    job = []
    for i, nm in enumerate(names):
        job.append(("good", nid(), site.url("/" + nm), outpath("cg"), nm, 1 + (i % 12)))
    bad = [("refused", f"http://127.0.0.1:{refused}/x"), ("drop", f"http://127.0.0.1:{drop.port}/x"),
           ("404", nf.url("/nope")), ("403", nf.url("/forbidden")), ("dns", "http://flux-nonexistent.invalid/x")]
    for kind, url in bad * 2:
        job.append((kind, nid(), url, outpath("cb"), None, 4))

    replies = {}
    gate = threading.Barrier(4)

    def worker(items):
        gate.wait()
        for kind, dl, url, dest, nm, segs in items:
            typ, _ = c.start(dl, url, dest, segments=segs)
            replies[dl] = typ

    chunks = [job[i::4] for i in range(4)]
    threads = [threading.Thread(target=worker, args=(ch,)) for ch in chunks]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    expect(all(replies.get(j[1]) == RSP_OK for j in job), f"all {len(job)} STARTs acknowledged", replies)
    ok_all, wrong = True, []
    for kind, dl, url, dest, nm, segs in job:
        res = c.wait_terminal(dl, 150)
        if kind == "good":
            verify(f"{nm} ({segs} segments)", c, dl, res, dest, nm) or (ok_all := False)
        else:
            want = {"refused": {ERR["NETWORK"]}, "drop": {ERR["NETWORK"], ERR["TIMEOUT"]}, "404": {ERR["NOT_FOUND"]},
                    "403": {ERR["FORBIDDEN"]}, "dns": {ERR["NETWORK"], ERR["TIMEOUT"]}}[kind]
            if not (res and res[0] == "error" and res[1]["code"] in want):
                wrong.append((kind, res))
    expect(not wrong, f"every failing download ends with its own error code ({len(bad) * 2} of them)", wrong)
    note(f"{len(job)} downloads in {time.time() - t0:.1f}s")
    expect(CORE.alive(), "core alive")
    clean_conn("not one corrupted or mis-flagged frame", c)
    rows = c.status_all()
    expect(rows is not None and len(rows) >= len(job), f"STATUS_ALL lists them ({0 if rows is None else len(rows)})")
    for kind, dl, *_ in job:
        c.cancel(dl)
    time.sleep(2.5)
    fd_after, thr_after = CORE.fd_count(), CORE.thread_count()
    expect(fd_after <= fd_before + 2, f"no fd leak (before {fd_before}, after {fd_after})")
    expect(thr_after <= thr_before + 1, f"worker threads all joined (before {thr_before}, after {thr_after})")
    c.close()
    site.stop()
    nf.stop()
    drop.stop()


def test_event_broadcast():
    info("Events reach every connected client; OK always precedes events")
    size = 3 << 20
    mkfile("bc.bin", size)
    site = Site(SRV)
    site.set("/bc.bin", chunk=32768, delay=0.02)
    a, b = CORE.connect(), CORE.connect()
    dl, dest = nid(), outpath()
    a.start_ok(dl, site.url("/bc.bin"), dest, segments=3)
    ra = a.wait_terminal(dl, 60)
    rb = b.wait_terminal(dl, 60)
    verify("client A (the one that started it)", a, dl, ra, dest, "bc.bin")
    expect(completed(rb), "client B (a bystander) also receives COMPLETE", rb)
    expect(b.dl(dl).nprogress > 0, "...and its progress frames")
    d = a.dl(dl)
    expect(d.first_reply_idx < d.first_event_idx, "the START reply comes before every event")
    c3 = CORE.connect()
    st = c3.status(dl)
    expect(isinstance(st, dict) and st["status"] == ST["COMPLETE"] and st["done"] == st["total"] == size,
           "a late client can ask for the finished download's STATUS", st)
    a.close(); b.close(); c3.close()
    site.stop()


def test_ipc_robustness():
    info("IPC robustness")
    good = CORE.connect()
    fd_before = CORE.fd_count()

    def closed_by_core(raw, wait=3.0):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(CORE.sock_path)
        s.sendall(raw)
        s.settimeout(wait)
        try:
            return s.recv(4096) == b""
        except socket.timeout:
            return False
        except OSError:
            return True
        finally:
            s.close()

    expect(closed_by_core(b"GET / HTTP/1.1\r\n\r\n" + b"x" * 64), "garbage bytes: connection dropped")
    expect(closed_by_core(struct.pack(HDR_FMT, MAGIC, VER, CMD_STATUS, 0, 1, 0xFFFFFFFF)), "absurd payload_len: connection dropped")
    expect(closed_by_core(struct.pack(HDR_FMT, MAGIC, 77, CMD_STATUS, 0, 1, 0)), "wrong protocol version: connection dropped")
    expect(CORE.alive() and isinstance(good.status(31337), tuple), "core and other clients unaffected")

    c = CORE.connect()
    typ, _ = c.request(0x55, 77)
    expect(typ == RSP_ERROR, "unknown command -> RSP_ERROR")
    typ, _ = c.request(CMD_START, 78, b"\x01\x02\x03")
    expect(typ == RSP_ERROR, "truncated START payload -> RSP_ERROR")
    body = struct.pack("<IHHQB32s7s", 79, 2, 0, 0, 0, b"\0" * 32, b"\0" * 7) + struct.pack("<H", 60000) + b"http://x"
    typ, _ = c.request(CMD_START, 79, body)
    expect(typ == RSP_ERROR, "START with url_len beyond the payload -> RSP_ERROR")
    typ, _ = c.request(CMD_SET_LIMIT, 0, b"\x01")
    expect(typ == RSP_ERROR, "SET_LIMIT with a short payload -> RSP_ERROR")
    typ, _ = c.request(CMD_STATUS, 80)
    expect(typ == RSP_ERROR, "STATUS of an unknown id -> RSP_ERROR")

    # Byte-at-a-time and coalesced frames
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(CORE.sock_path)
    frame = encode_frame(CMD_STATUS_ALL, 0)
    for i in range(len(frame)):
        s.sendall(frame[i:i + 1])
        time.sleep(0.002)
    s.sendall(frame * 5)
    s.settimeout(5)
    got_ok, buf = 0, b""
    deadline = time.time() + 5
    while got_ok < 6 and time.time() < deadline:
        try:
            buf += s.recv(65536)
        except socket.timeout:
            break
        while len(buf) >= HDR_SIZE:
            _m, _v, t, _f, _i, plen = struct.unpack(HDR_FMT, buf[:HDR_SIZE])
            if len(buf) < HDR_SIZE + plen:
                break
            if t == RSP_OK:
                got_ok += 1
            buf = buf[HDR_SIZE + plen:]
    expect(got_ok == 6, f"split and coalesced frames are reassembled (got {got_ok}/6 replies)")
    s.close()

    # Connect / disconnect churn, half frames. The core may refuse a connection while it
    # has not yet noticed that earlier ones went away (16 client slots), so errors are fine.
    for i in range(150):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(CORE.sock_path)
            if i % 3 == 0:
                s.sendall(encode_frame(CMD_STATUS_ALL, 0)[:7])
        except OSError:
            pass
        s.close()
        if i % 6 == 5:
            time.sleep(0.01)
    time.sleep(0.5)
    expect(CORE.alive() and CORE.fd_count() <= fd_before + 3, f"no fd leak after 150 connections (before {fd_before}, now {CORE.fd_count()})")

    # Client limit: the 17th connection is refused, a slot frees up again
    conns = []
    try:
        for _ in range(15):
            conns.append(CORE.connect())
        extra = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        extra.connect(CORE.sock_path)
        extra.settimeout(3)
        try:
            refused = extra.recv(16) == b""
        except socket.timeout:
            refused = False
        except OSError:
            refused = True
        extra.close()
        expect(refused, "connections beyond the client limit are closed")
    finally:
        for x in conns:
            x.close()
    time.sleep(0.3)
    c4 = CORE.connect()
    expect(isinstance(c4.status(1), tuple), "a slot is available again after clients leave")
    c4.close()
    clean_conn("long-lived client saw only well-formed frames", good)
    good.close()
    c.close()


def test_slow_reader():
    info("A client that never reads must not hurt anyone else")
    site = Site(SRV)
    mkfile("sr.bin", 6 << 20)
    site.set("/sr.bin", chunk=32768, delay=0.01)
    healthy = CORE.connect()
    # make the registry big, so that STATUS_ALL answers are large
    for i in range(60):
        dl = nid()
        (SRV / "tiny.bin").write_bytes(b"t")
        FILES["tiny.bin"] = (1, hashlib.sha256(b"t").hexdigest())
        healthy.start_ok(dl, site.url("/tiny.bin"), outpath("t"), segments=1)
        healthy.wait_terminal(dl, 20)

    stuck = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    stuck.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    stuck.connect(CORE.sock_path)
    req = encode_frame(CMD_STATUS_ALL, 0)
    t0 = time.time()
    sent = 0
    stuck.setblocking(False)
    try:
        for _ in range(6000):
            try:
                stuck.send(req)
                sent += 1
            except BlockingIOError:
                time.sleep(0.001)
            except OSError:
                break
    finally:
        pass
    dl, dest = nid(), outpath()
    healthy.start_ok(dl, site.url("/sr.bin"), dest, segments=4)
    res = healthy.wait_terminal(dl, 90)
    verify("another client's download is unaffected by the stuck reader", healthy, dl, res, dest, "sr.bin")
    # Everything it did not read is queued inside the core; past the hard limit it is cut off.
    stuck.setblocking(True)
    stuck.settimeout(5)
    eof, nread = False, 0
    try:
        while True:
            blk = stuck.recv(1 << 20)
            if not blk:
                eof = True
                break
            nread += len(blk)
            if nread > 64 << 20:
                break
    except OSError:
        eof = True                                    # ECONNRESET counts as being cut off
    expect(eof, f"the client that fell hopelessly behind was disconnected (read {nread >> 10} KiB first)")
    typ, _ = healthy.request(CMD_STATUS, dl)
    expect(typ == RSP_STATUS, "...and it still gets prompt answers")
    time.sleep(0.5)
    expect(CORE.alive(), f"core alive after {sent} unread STATUS_ALL requests ({time.time() - t0:.1f}s)")
    rss = CORE.rss_kb()
    expect(0 < rss < 400_000, f"core memory stays bounded (RSS {rss // 1024} MiB)")
    stuck.close()
    healthy.close()
    site.stop()


def test_client_disconnect_midway():
    info("A client that disconnects mid-download does not stop it")
    size = 4 << 20
    mkfile("dm.bin", size)
    site = Site(SRV)
    site.set("/dm.bin", chunk=32768, delay=0.02)
    a = CORE.connect()
    dl, dest = nid(), outpath()
    a.start_ok(dl, site.url("/dm.bin"), dest, segments=4)
    a.wait_done(dl, 1, 15)
    a.close()
    time.sleep(0.2)
    b = CORE.connect()
    st = b.wait_status(dl, ST["COMPLETE"], 60)
    expect(isinstance(st, dict) and st["status"] == ST["COMPLETE"], "download completes without its client", st)
    verify("...and the file is right", b, dl, ("complete", {"total": size}), dest, "dm.bin")
    b.close()
    site.stop()


# ═══ HTTPS ════════════════════════════════════════════════════════════════════
def test_https():
    info("HTTPS: local CA, hostname and IP verification")
    pki = make_ca_and_cert(ROOT / "pki", names=("localhost",), ips=("127.0.0.1",))
    if pki is None:
        warn("openssl command not found - skipping the HTTPS tests")
        return
    ca, cert, key = pki
    rogue = make_ca_and_cert(ROOT / "rogue", names=("localhost",), ips=("127.0.0.1",))
    wrongname = make_ca_and_cert(ROOT / "wrongname", names=("other.example",), ips=())
    dnsonly = make_ca_and_cert(ROOT / "dnsonly", names=("localhost",), ips=())
    # the odd certificates are signed by their OWN CAs; trust all of them so that only the NAME can be wrong
    bundle = ROOT / "bundle.pem"
    bundle.write_bytes(b"".join(Path(p[0]).read_bytes() for p in (pki, wrongname, dnsonly)))

    core = fresh_core(env={"SSL_CERT_FILE": str(bundle)})
    c = core.connect()
    size = 20 << 20
    mkfile("tls.bin", size)
    site = Site(SRV, tls=(cert, key))
    dl, res, dest = fetch(c, site, "/tls.bin", segments=8)
    verify("https://127.0.0.1 (IP SAN), 20 MiB, 8 segments", c, dl, res, dest, "tls.bin")
    nloc = site.url("/tls.bin", host="localhost")
    dl = nid()
    dest = outpath()
    c.start_ok(dl, nloc, dest, segments=8)
    verify("https://localhost (DNS SAN)", c, dl, c.wait_terminal(dl, 90), dest, "tls.bin")

    site.set("/tls_ka.bin", keepalive=True)
    mkfile("tls_ka.bin", 3 << 20)
    site.set("/tls_ch.bin", ranges=False, chunked=True)
    mkfile("tls_ch.bin", 2 << 20)
    dl, res, dest = fetch(c, site, "/tls_ka.bin", segments=4)
    verify("TLS + keep-alive server", c, dl, res, dest, "tls_ka.bin")
    dl, res, dest = fetch(c, site, "/tls_ch.bin", segments=4)
    verify("TLS + chunked single stream", c, dl, res, dest, "tls_ch.bin")

    r_site = Site(SRV, tls=(rogue[1], rogue[2]))
    dl, res, dest = fetch(c, r_site, "/tls.bin", segments=4, timeout=60)
    expect(res and res[0] == "error" and res[1]["code"] == ERR["TLS"],
           "certificate from an untrusted CA is refused (TLS error)", res)
    expect(not dest.exists(), "...and nothing was written")
    expect(not r_site.requests(), "...the HTTP request was never sent over the unverified channel", r_site.requests())

    w_site = Site(SRV, tls=(wrongname[1], wrongname[2]))
    dl, res, dest = fetch(c, w_site, "/tls.bin", segments=4, timeout=60)
    expect(res and res[0] == "error" and res[1]["code"] == ERR["TLS"],
           "trusted certificate for the WRONG host name is refused", res)
    expect(not w_site.requests(), "...no request leaked")

    d_site = Site(SRV, tls=(dnsonly[1], dnsonly[2]))
    dl, res, dest = fetch(c, d_site, "/tls.bin", segments=4, timeout=60)               # by IP address
    expect(res and res[0] == "error" and res[1]["code"] == ERR["TLS"],
           "an IP address is NOT matched against a certificate that only names 'localhost'", res)
    expect(not d_site.requests(), "...no request leaked")
    dl = nid()
    dest = outpath()
    c.start_ok(dl, d_site.url("/tls.bin", host="localhost"), dest, segments=4)
    verify("...while the same certificate is fine for https://localhost", c, dl, c.wait_terminal(dl, 60), dest, "tls.bin")

    clean_conn("connection clean", c)
    c.close()
    expect(core.stop() == 0, "core exits cleanly")
    shutil.rmtree(core.dir, ignore_errors=True)
    for s_ in (site, r_site, w_site, d_site):
        s_.stop()


# ═══ Throughput (informational) ═══════════════════════════════════════════════
def test_throughput():
    info("Throughput over loopback (informational; the Python test server is the bottleneck)")
    size = (64 if QUICK else 192) << 20
    mkfile("speed.bin", size)
    site = Site(SRV, chunk=1 << 20)
    c = CORE.connect()
    for segs in (1, 8):
        t0 = time.time()
        dl, res, dest = fetch(c, site, "/speed.bin", segments=segs, timeout=180)
        dt = time.time() - t0
        verify(f"{size >> 20} MiB with {segs} segment(s): {size / dt / 1e6:.0f} MB/s", c, dl, res, dest, "speed.bin")
        expect(dt < 60, f"...in a sane time ({dt:.1f}s)")
        try:
            dest.unlink()
        except OSError:
            pass
    c.close()
    site.stop()


# ═══ Main ═════════════════════════════════════════════════════════════════════
TESTS = [
    test_identity, test_sizes_and_segments, test_empty_file, test_server_flavours, test_url_forms, test_redirects,
    test_basic_auth, test_checksums, test_failure_paths, test_retry_policies, test_rate_limits,
    test_rate_limit_patience, test_rate_limit_queue, test_disk_errors,
    test_pause_resume, test_pause_resume_storm, test_cancel_cleanup, test_interruptible_probe,
    test_bandwidth_limits, test_duplicates_and_ids, test_resume_after_shutdown, test_resume_after_sigterm,
    test_resume_after_sigkill, test_resume_validation, test_second_instance, test_concurrent_downloads,
    test_event_broadcast, test_ipc_robustness, test_slow_reader, test_client_disconnect_midway,
    test_https, test_throughput,
]


def main():
    print(f"\n{BOLD}{CYAN} FluxDM — Core behaviour tests{RESET}")
    print(f"{DIM}  core     : {CORE.binary}\n  temp dir : {ROOT}{RESET}")
    if not Path(CORE.binary).exists():
        warn(f"fluxdm-core not found at {CORE.binary} — build it first: cd core && make")
        return 1
    if not CORE.start():
        warn("the core did not start:\n" + CORE.log_text())
        return 1
    fd0, th0 = CORE.fd_count(), CORE.thread_count()
    tests = [t for t in TESTS if ONLY is None or ONLY in t.__name__]
    ok_ = False

    def finish():
        time.sleep(2.0)
        expect(CORE.alive(), "core survived the whole suite")
        fd1, th1 = CORE.fd_count(), CORE.thread_count()
        expect(fd1 <= fd0 + 3, f"no descriptor leak over the whole suite ({fd0} -> {fd1})")
        expect(th1 <= th0 + 1, f"no thread leak over the whole suite ({th0} -> {th1})")
        rc = CORE.stop()
        expect(rc == 0, f"core exits cleanly on SHUTDOWN (exit code {rc})")
        log = CORE.log_text()
        bad = [l for l in log.splitlines()
               if re.search(r"Sanitizer|runtime error|joincheck|ASAN|Assertion|double free|corrupt", l)]
        expect(not bad, "no sanitizer / assertion / joincheck reports in the core's log", "\n".join(bad[:10]))

    try:
        ok_ = run_tests("Core Behaviour Test Results", tests, after=finish)
    finally:
        CORE.stop()
        if not ok_:
            print(f"{DIM}core log kept at {CORE.log_path}{RESET}")
            tail = CORE.log_text().strip().splitlines()[-25:]
            print("\n".join("    " + l for l in tail))
        else:
            shutil.rmtree(CORE.dir, ignore_errors=True)
        shutil.rmtree(ROOT, ignore_errors=True)
    return 0 if ok_ else 1


if __name__ == "__main__":
    sys.exit(main())
