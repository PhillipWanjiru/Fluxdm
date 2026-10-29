#!/usr/bin/env python3
"""
FluxDM — tests/test_integration.py
Smoke / integration tests for the C core daemon.

Starts a private fluxdm-core on its own socket (so a daemon you are running is
never touched), serves files from a range-aware local HTTP server and drives the
core through the real IPC protocol.

  FLUXDM_CORE=/path/to/fluxdm-core  python3 tests/test_integration.py

By default the binary is  <project>/core/fluxdm-core  (relative to this file).
"""
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fluxtest import *                                              # noqa: E402,F403

ROOT = tmpdir("fluxint_")
CORE = Core()
SITE = None
_next = [100]


def next_id():
    _next[0] += 1
    return _next[0]


# ── Tests ─────────────────────────────────────────────────────────────────────
def test_core_starts():
    info("Test 1 — Core starts and binds its socket")
    if not Path(CORE.binary).exists():
        warn(f"fluxdm-core not found at {CORE.binary}")
        warn("Build it first:  cd core && make")
        return False
    ok_ = CORE.start()
    expect(ok_, "fluxdm-core started", CORE.log_text())
    if not ok_:
        return False
    expect(os.path.exists(CORE.sock_path), "IPC socket exists")
    mode = stat.S_IMODE(os.stat(CORE.sock_path).st_mode)
    expect(mode == 0o600, f"IPC socket is private (mode {mode:04o})")
    return True


def test_ipc_connect():
    info("Test 2 — IPC connection accepted")
    try:
        c = CORE.connect()
        expect(True, "Connected to the Unix socket")
        c.close()
    except OSError as e:
        expect(False, f"IPC connect failed: {e}")


def test_simple_download():
    info("Test 3 — CMD_START: download 1 MB (4 segments)")
    sha = make_file(ROOT, "small.bin", 1 << 20)
    dest = ROOT / "small_out.bin"
    c, dl = CORE.connect(), next_id()

    typ, pl = c.start(dl, SITE.url("/small.bin"), dest, segments=4)
    expect(typ is not None, "Got a response to CMD_START")
    expect(typ == RSP_OK, f"RSP_OK for CMD_START (got {NAMES.get(typ, typ)})",
           parse_error(pl) if typ == RSP_ERROR else None)

    res = c.wait_terminal(dl, 45)
    expect(res is not None and res[0] == "complete", "RSP_COMPLETE received", res)
    if res and res[0] == "complete":
        expect(res[1]["total"] == 1 << 20, "COMPLETE reports the file size")
        expect(dest.exists(), "Downloaded file exists on disk")
        if dest.exists():
            expect(dest.stat().st_size == 1 << 20, f"File size correct ({dest.stat().st_size} bytes)")
            expect(sha256_file(dest) == sha, "SHA-256 matches the source file")
        expect(not Path(str(dest) + ".fluxpart").exists(), "No .fluxpart left behind")
    expect(not c.proto_errors, "No protocol errors on the connection", c.proto_errors)
    c.close()


def test_status_command():
    info("Test 4 — CMD_STATUS during a download")
    make_file(ROOT, "medium.bin", 5 << 20)
    SITE.set("/medium.bin", chunk=16384, delay=0.05)
    c, dl = CORE.connect(), next_id()
    expect(c.start_ok(dl, SITE.url("/medium.bin"), ROOT / "medium_out.bin", segments=8),
           "START acknowledged")
    c.wait_done(dl, 1, 15)
    st = c.status(dl)
    expect(isinstance(st, dict), "RSP_STATUS received", st)
    if isinstance(st, dict):
        expect(st["id"] == dl, f"Status ID matches (got {st['id']})")
        expect(st["status"] in (ST["CONNECTING"], ST["DOWNLOADING"], ST["COMPLETE"]),
               f"Status is a live state (got {st['status']})")
        expect(st["total"] == 5 << 20, "Status reports the total size")
        expect(st["done"] <= st["total"], "Downloaded <= total")
    res = c.wait_terminal(dl, 60)
    expect(res is not None and res[0] == "complete", "Download finished")
    SITE.set("/medium.bin")
    c.close()


def test_progress_frames():
    info("Test 5 — RSP_PROGRESS frames during a 10 MB download")
    make_file(ROOT, "large.bin", 10 << 20)
    SITE.set("/large.bin", chunk=32768, delay=0.03)
    c, dl = CORE.connect(), next_id()
    c.start_ok(dl, SITE.url("/large.bin"), ROOT / "large_out.bin", segments=8)
    res = c.wait_terminal(dl, 60)
    d = c.dl(dl)
    expect(res is not None and res[0] == "complete", "Download finished", res)
    expect(d.nprogress > 0, f"Received {d.nprogress} RSP_PROGRESS frames")
    expect(d.backwards == 0, "Progress never moved backwards")
    expect(d.over_total == 0, "Progress never exceeded the total")
    expect(c.unflagged_events == 0, "Every progress/complete frame carries the EVENT flag")
    SITE.set("/large.bin")
    c.close()


def test_set_limit():
    info("Test 6 — CMD_SET_LIMIT (global bandwidth cap)")
    c = CORE.connect()
    expect(c.set_limit(0, 512 * 1024) is True, "RSP_OK for CMD_SET_LIMIT")
    expect(c.set_limit(0, 0) is True, "Limit can be lifted again")
    r = c.set_limit(987654, 1000)
    expect(isinstance(r, tuple) and r[0] == ERR["NOT_FOUND"], "SET_LIMIT for an unknown id -> NOT_FOUND", r)
    c.close()


def test_cancel():
    info("Test 7 — CMD_CANCEL stops a download and cleans up")
    make_file(ROOT, "cancel.bin", 20 << 20)
    SITE.set("/cancel.bin", chunk=16384, delay=0.01)
    dest = ROOT / "cancel_out.bin"
    c, dl = CORE.connect(), next_id()
    c.start_ok(dl, SITE.url("/cancel.bin"), dest, segments=4)
    c.wait_done(dl, 1, 15)
    expect(c.cancel(dl) is True, "RSP_OK for CMD_CANCEL")
    time.sleep(0.4)
    expect(not Path(str(dest) + ".fluxpart").exists(), ".fluxpart removed after cancel")
    expect(not dest.exists(), "partial data file removed after cancel")
    expect(c.cancel(dl) is True, "CANCEL is idempotent")
    SITE.set("/cancel.bin")
    c.close()


def test_status_all():
    info("Test 8 — CMD_STATUS_ALL")
    c = CORE.connect()
    rows = c.status_all()
    expect(rows is not None, "CMD_STATUS_ALL ends with RSP_OK")
    expect(rows is not None and all(isinstance(r["id"], int) for r in rows), "STATUS rows are well-formed")
    c.close()


def test_default_socket():
    info("Test 9 — Without --sock the core listens inside a private directory")
    home = Path(tempfile.mkdtemp(prefix="fxh", dir="/tmp"))            # short: sun_path is 108 bytes
    xdg = home / "run"
    xdg.mkdir()
    os.chmod(xdg, 0o700)
    cases = [("with XDG_RUNTIME_DIR", {"XDG_RUNTIME_DIR": str(xdg), "HOME": str(home)}, xdg / "fluxdm"),
             ("without XDG_RUNTIME_DIR", {"HOME": str(home)}, home / ".fluxdm" / "run")]
    try:
        for label, extra, run_dir in cases:
            env = {k: v for k, v in os.environ.items() if k != "XDG_RUNTIME_DIR"}
            env.update(extra)
            sock = run_dir / "core.sock"
            proc = subprocess.Popen([CORE.binary, "--log", "0"], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                up = False
                for _ in range(200):
                    if sock.exists() or proc.poll() is not None:
                        break
                    time.sleep(0.05)
                up = sock.exists() and proc.poll() is None
                expect(up, f"{label}: the socket appears at {sock.relative_to(home)}")
                if up:
                    expect(stat.S_IMODE(os.stat(run_dir).st_mode) == 0o700, f"{label}: its directory is private")
                    expect(stat.S_IMODE(os.stat(sock).st_mode) == 0o600, f"{label}: the socket is private")
                    c = Conn(str(sock))
                    expect(c.status_all() is not None, f"{label}: the core answers on it")
                    c.close()
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            expect(not sock.exists(), f"{label}: the socket is removed when the core stops")
    finally:
        shutil.rmtree(home, ignore_errors=True)


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    global SITE
    print(f"\n{BOLD}{CYAN} FluxDM — Integration Tests{RESET}")
    print(f"{DIM}  core     : {CORE.binary}\n  temp dir : {ROOT}{RESET}")
    SITE = Site(ROOT)
    ok_ = False
    try:
        if not test_core_starts():
            warn("Skipping the remaining tests — the core did not start.")
            return 1
        tests = [test_ipc_connect, test_simple_download, test_status_command,
                 test_progress_frames, test_set_limit, test_cancel, test_status_all,
                 test_default_socket]
        def finish():
            rc = CORE.stop()
            expect(rc == 0, f"Core exits cleanly on shutdown (exit code {rc})", CORE.log_text())

        ok_ = run_tests("Integration Test Results", tests, after=finish)
    finally:
        CORE.stop()
        SITE.stop()
        import shutil
        shutil.rmtree(ROOT, ignore_errors=True)
        shutil.rmtree(CORE.dir, ignore_errors=True)
    return 0 if ok_ else 1


if __name__ == "__main__":
    sys.exit(main())
