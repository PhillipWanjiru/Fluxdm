#!/usr/bin/env python3
"""
FluxDM — tests/test_integration.py
Integration tests for the Phase 1 C core.
Uses a range-aware HTTP server so multi-segment downloads work correctly.
"""

import os, sys, socket, struct, time, hashlib, threading
import subprocess, tempfile
from pathlib import Path
from http.server import BaseHTTPRequestHandler, HTTPServer

# ── Config ────────────────────────────────────────────────────────────────────
PROJECT  = Path.home() / "Desktop" / "fluxdm"
CORE_BIN = PROJECT / "core" / "fluxdm-core"
SOCK_PATH = "/tmp/fluxdm.sock"
HTTP_PORT = 17171
TMPDIR    = Path(tempfile.mkdtemp(prefix="fluxdm_test_"))

# ── Colors ────────────────────────────────────────────────────────────────────
GREEN = "\033[92m"; RED = "\033[91m"; YELLOW = "\033[93m"
CYAN  = "\033[96m"; BOLD = "\033[1m"; DIM = "\033[2m"; RESET = "\033[0m"

pass_count = 0
fail_count = 0

def ok(msg):   print(f"  {GREEN}✔{RESET}  {msg}")
def fail(msg): print(f"  {RED}✘{RESET}  {msg}"); global fail_count; fail_count += 1
def info(msg): print(f"\n{BOLD}{CYAN}── {msg}{RESET}")
def warn(msg): print(f"  {YELLOW}⚠{RESET}  {msg}")

def expect(cond, msg):
    global pass_count, fail_count
    if cond: ok(msg);   pass_count += 1
    else:    fail(msg); fail_count += 1

# ── IPC protocol ──────────────────────────────────────────────────────────────
FLUX_MAGIC   = 0x464C5558
FLUX_VER     = 1
CMD_START    = 0x01; CMD_PAUSE   = 0x02; CMD_RESUME = 0x03
CMD_CANCEL   = 0x04; CMD_SET_LIMIT = 0x05
CMD_STATUS   = 0x06; CMD_STATUS_ALL = 0x07; CMD_SHUTDOWN = 0xFF
RSP_OK       = 0x10; RSP_ERROR   = 0x11; RSP_PROGRESS = 0x12
RSP_COMPLETE = 0x13; RSP_STATUS  = 0x14

HDR_FMT  = "<IBBHII"
HDR_SIZE = struct.calcsize(HDR_FMT)   # 16

def encode_hdr(typ, dl_id, plen):
    return struct.pack(HDR_FMT, FLUX_MAGIC, FLUX_VER, typ, 0, dl_id, plen)

def decode_hdr(data):
    magic, ver, typ, flags, dl_id, plen = struct.unpack(HDR_FMT, data)
    return {"magic": magic, "ver": ver, "type": typ, "id": dl_id, "payload_len": plen}

def send_cmd(sock, typ, dl_id, payload=b""):
    sock.sendall(encode_hdr(typ, dl_id, len(payload)) + payload)

def recv_frame(sock, timeout=10.0):
    sock.settimeout(timeout)
    raw = b""
    try:
        while len(raw) < HDR_SIZE:
            chunk = sock.recv(HDR_SIZE - len(raw))
            if not chunk: return None, None
            raw += chunk
    except (TimeoutError, OSError):
        return None, None
    hdr  = decode_hdr(raw)
    plen = hdr["payload_len"]
    payload = b""
    try:
        while len(payload) < plen:
            chunk = sock.recv(plen - len(payload))
            if not chunk: break
            payload += chunk
    except (TimeoutError, OSError):
        pass
    return hdr, payload

def build_start_payload(dl_id, url, dest_path, segments=8, bw_limit=0,
                         cksum_type=0, cksum=b"\x00"*32):
    # flux_cmd_start_t packed (56 bytes):
    #   uint32 id, uint16 segments, uint16 flags, uint64 bw_limit,
    #   uint8 cksum_type, uint8[32] checksum, uint8[7] _pad
    body  = struct.pack("<IHHQB32s7s",
                        dl_id, segments, 0, bw_limit, cksum_type,
                        (cksum + b"\x00" * 32)[:32],
                        b"\x00" * 7)
    url_b  = url.encode()
    path_b = dest_path.encode()
    body  += struct.pack("<H", len(url_b))  + url_b
    body  += struct.pack("<H", len(path_b)) + path_b
    return body

def wait_for_complete(sock, timeout_secs=45):
    """Drain frames until RSP_COMPLETE or RSP_ERROR. Returns (success, msg)."""
    deadline = time.time() + timeout_secs
    while time.time() < deadline:
        remaining = deadline - time.time()
        hdr, pl = recv_frame(sock, timeout=min(0.5, remaining))
        if hdr is None: continue
        if hdr["type"] == RSP_COMPLETE:
            return True, "RSP_COMPLETE received"
        if hdr["type"] == RSP_ERROR:
            code = pl[0] if pl else 0
            msg  = pl[4:].decode(errors="replace").rstrip("\x00") if len(pl) > 4 else ""
            return False, f"RSP_ERROR code={code}: {msg}"
    return False, f"Timeout after {timeout_secs}s"

# ── Range-aware HTTP server ───────────────────────────────────────────────────
class RangeHandler(BaseHTTPRequestHandler):
    """Serves files with full Accept-Ranges + 206 Partial Content support."""

    def log_message(self, *args): pass   # suppress noisy access logs

    def do_HEAD(self):
        path = TMPDIR / self.path.lstrip("/")
        if not path.exists():
            self.send_error(404); return
        size = path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()

    def do_GET(self):
        path = TMPDIR / self.path.lstrip("/")
        if not path.exists():
            self.send_error(404); return

        size      = path.stat().st_size
        range_hdr = self.headers.get("Range")

        if range_hdr:
            try:
                byte_range = range_hdr.replace("bytes=", "").strip()
                parts      = byte_range.split("-")
                start      = int(parts[0]) if parts[0] else 0
                end        = int(parts[1]) if parts[1] else size - 1
                end        = min(end, size - 1)
                length     = end - start + 1

                self.send_response(206)
                self.send_header("Content-Range",
                                 f"bytes {start}-{end}/{size}")
                self.send_header("Content-Length", str(length))
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Connection", "keep-alive")
                self.end_headers()

                with open(path, "rb") as f:
                    f.seek(start)
                    remaining = length
                    while remaining > 0:
                        chunk = f.read(min(65536, remaining))
                        if not chunk: break
                        try: self.wfile.write(chunk)
                        except (BrokenPipeError, ConnectionResetError): break
                        remaining -= len(chunk)
            except Exception:
                self.send_error(400)
        else:
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk: break
                    try: self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError): break

http_server = None
http_thread = None

def start_http_server():
    global http_server, http_thread
    http_server = HTTPServer(("127.0.0.1", HTTP_PORT), RangeHandler)
    http_server.allow_reuse_address = True
    http_thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    http_thread.start()

def stop_http_server():
    if http_server: http_server.shutdown()

# ── Test file helpers ─────────────────────────────────────────────────────────
_dl_counter = 100   # unique download IDs

def next_id():
    global _dl_counter
    _dl_counter += 1
    return _dl_counter

def make_test_file(name, size_bytes):
    path = TMPDIR / name
    data = (bytes(range(256)) * (size_bytes // 256 + 1))[:size_bytes]
    path.write_bytes(data)
    digest = hashlib.sha256(data).digest()
    return str(path), digest

# ── Core process management ───────────────────────────────────────────────────
core_proc = None

def start_core():
    global core_proc
    if not CORE_BIN.exists():
        warn(f"fluxdm-core not found at {CORE_BIN}")
        warn("Build it first: cd ~/Desktop/fluxdm/core && make")
        return False
    if Path(SOCK_PATH).exists():
        os.unlink(SOCK_PATH)
    core_proc = subprocess.Popen(
        [str(CORE_BIN), "--log", "0"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(50):
        if Path(SOCK_PATH).exists(): return True
        time.sleep(0.1)
    return False

def stop_core():
    global core_proc
    if core_proc:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK_PATH)
            send_cmd(s, CMD_SHUTDOWN, 0)
            s.close()
        except Exception: pass
        time.sleep(0.3)
        core_proc.terminate()
        try: core_proc.wait(timeout=3)
        except subprocess.TimeoutExpired: core_proc.kill()
        core_proc = None

def connect_ipc():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)
    return s

# ── Tests ─────────────────────────────────────────────────────────────────────

def test_core_starts():
    info("Test 1 — Core starts and binds socket")
    result = start_core()
    expect(result,                        "fluxdm-core started")
    expect(Path(SOCK_PATH).exists(),      f"{SOCK_PATH} exists")
    return result

def test_ipc_connect():
    info("Test 2 — IPC connection accepted")
    try:
        s = connect_ipc()
        expect(True, "Connected to Unix socket")
        s.close()
        return True
    except Exception as e:
        expect(False, f"IPC connect failed: {e}")
        return False

def test_simple_download():
    info("Test 3 — CMD_START: download 1 MB file (4 segments)")
    src, sha256 = make_test_file("small.bin", 1024 * 1024)
    dest = str(TMPDIR / "small_out.bin")
    url  = f"http://127.0.0.1:{HTTP_PORT}/small.bin"
    dl_id = next_id()

    s = connect_ipc()
    payload = build_start_payload(dl_id, url, dest, segments=4)
    send_cmd(s, CMD_START, dl_id, payload)

    hdr, pl = recv_frame(s, timeout=5)
    expect(hdr is not None, "Got response to CMD_START")
    if hdr and hdr["type"] == RSP_ERROR:
        code = pl[0] if pl else 0
        msg  = pl[4:].decode(errors="replace").rstrip("\x00") if len(pl) > 4 else ""
        expect(False, f"RSP_ERROR code={code}: {msg}")
        s.close(); return
    ok_type = hdr is not None and hdr["type"] == RSP_OK
    expect(ok_type, f"RSP_OK (got {'None' if not hdr else hex(hdr['type'])})")

    ok_flag, msg = wait_for_complete(s, timeout_secs=45)
    expect(ok_flag, msg)

    if ok_flag:
        dest_path = Path(dest)
        expect(dest_path.exists(), "Downloaded file exists on disk")
        if dest_path.exists():
            actual_size = dest_path.stat().st_size
            expect(actual_size == 1024 * 1024,
                   f"File size correct ({actual_size} bytes)")
            actual_sha = hashlib.sha256(dest_path.read_bytes()).digest()
            expect(actual_sha == sha256, "SHA-256 matches source file")
    s.close()

def test_status_command():
    info("Test 4 — CMD_STATUS during 5 MB download")
    src, _ = make_test_file("medium.bin", 5 * 1024 * 1024)
    dest   = str(TMPDIR / "medium_out.bin")
    url    = f"http://127.0.0.1:{HTTP_PORT}/medium.bin"
    dl_id  = next_id()

    s = connect_ipc()
    payload = build_start_payload(dl_id, url, dest, segments=8)
    send_cmd(s, CMD_START, dl_id, payload)
    hdr, pl = recv_frame(s, timeout=5)

    if hdr and hdr["type"] == RSP_ERROR:
        code = pl[0] if pl else 0
        msg  = pl[4:].decode(errors="replace").rstrip("\x00") if len(pl) > 4 else ""
        expect(False, f"CMD_START RSP_ERROR code={code}: {msg}")
        s.close(); return

    time.sleep(0.2)
    send_cmd(s, CMD_STATUS, dl_id)
    status_received = False
    for _ in range(20):
        hdr, pl = recv_frame(s, timeout=1.0)
        if hdr is None: continue
        if hdr["type"] == RSP_STATUS:
            status_received = True
            dl_id_back = struct.unpack_from("<I", pl)[0] if len(pl) >= 4 else 0
            expect(dl_id_back == dl_id, f"Status ID matches (got {dl_id_back})")
            break

    expect(status_received, "RSP_STATUS received")
    wait_for_complete(s, timeout_secs=45)
    s.close()

def test_progress_frames():
    info("Test 5 — RSP_PROGRESS frames during 10 MB download")
    src, _ = make_test_file("large.bin", 10 * 1024 * 1024)
    dest   = str(TMPDIR / "large_out.bin")
    url    = f"http://127.0.0.1:{HTTP_PORT}/large.bin"
    dl_id  = next_id()

    s = connect_ipc()
    payload = build_start_payload(dl_id, url, dest, segments=8)
    send_cmd(s, CMD_START, dl_id, payload)
    recv_frame(s, timeout=5)   # RSP_OK

    progress_count = 0
    deadline = time.time() + 45
    while time.time() < deadline:
        hdr, _ = recv_frame(s, timeout=0.5)
        if hdr is None: continue
        if hdr["type"] == RSP_PROGRESS: progress_count += 1
        if hdr["type"] in (RSP_COMPLETE, RSP_ERROR): break

    expect(progress_count > 0, f"Received {progress_count} RSP_PROGRESS frames")
    s.close()

def test_set_limit():
    info("Test 6 — CMD_SET_LIMIT (global bandwidth cap)")
    s = connect_ipc()
    payload = struct.pack("<Q", 512 * 1024)
    send_cmd(s, CMD_SET_LIMIT, 0, payload)
    hdr, _ = recv_frame(s, timeout=5)
    expect(hdr is not None and hdr["type"] == RSP_OK,
           "RSP_OK for CMD_SET_LIMIT")
    send_cmd(s, CMD_SET_LIMIT, 0, struct.pack("<Q", 0))
    recv_frame(s, timeout=5)
    s.close()

def test_cancel():
    info("Test 7 — CMD_CANCEL stops download")
    src, _ = make_test_file("cancel.bin", 20 * 1024 * 1024)
    dest   = str(TMPDIR / "cancel_out.bin")
    url    = f"http://127.0.0.1:{HTTP_PORT}/cancel.bin"
    dl_id  = next_id()

    s = connect_ipc()
    payload = build_start_payload(dl_id, url, dest, segments=4)
    send_cmd(s, CMD_START, dl_id, payload)
    recv_frame(s, timeout=5)

    time.sleep(0.2)
    send_cmd(s, CMD_CANCEL, dl_id)
    hdr, _ = recv_frame(s, timeout=5)
    expect(hdr is not None and hdr["type"] == RSP_OK,
           "RSP_OK for CMD_CANCEL")

    time.sleep(0.5)
    expect(not Path(dest + ".fluxpart").exists(),
           ".fluxpart removed after cancel")
    s.close()

def test_status_all():
    info("Test 8 — CMD_STATUS_ALL")
    s = connect_ipc()
    send_cmd(s, CMD_STATUS_ALL, 0)
    got_ok = False
    for _ in range(30):
        hdr, _ = recv_frame(s, timeout=1.0)
        if hdr is None: break
        if hdr["type"] == RSP_OK: got_ok = True; break
    expect(got_ok, "CMD_STATUS_ALL ends with RSP_OK")
    s.close()

# ── Summary ───────────────────────────────────────────────────────────────────
def summary():
    total = pass_count + fail_count
    print(f"\n{BOLD}{CYAN}────────────────────────────────────────────────{RESET}")
    print(f"{BOLD}  Integration Test Results{RESET}")
    print(f"{CYAN}────────────────────────────────────────────────{RESET}")
    print(f"  {GREEN}✔  Passed : {pass_count}{RESET}")
    print(f"  {RED if fail_count else DIM}✘  Failed : {fail_count}{RESET}")
    print(f"     Total  : {total}")
    print(f"{CYAN}────────────────────────────────────────────────{RESET}")
    if fail_count == 0:
        print(f"  {BOLD}{GREEN}ALL INTEGRATION TESTS PASSED{RESET}\n")
    else:
        print(f"  {BOLD}{RED}SOME TESTS FAILED{RESET}\n")
    return fail_count == 0

# ── Main ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"\n{BOLD}{CYAN} FluxDM Phase 1 — Integration Tests{RESET}")
    print(f"{DIM}  Temp dir : {TMPDIR}{RESET}")

    start_http_server()
    ok(f"HTTP test server started on port {HTTP_PORT} (range-aware)")

    if not test_core_starts():
        warn("Skipping integration tests — build core first:")
        warn(f"  cd {PROJECT}/core && make")
        stop_http_server()
        sys.exit(1)

    try:
        test_ipc_connect()
        test_simple_download()
        test_status_command()
        test_progress_frames()
        test_set_limit()
        test_cancel()
        test_status_all()
    finally:
        stop_core()
        stop_http_server()
        import shutil
        shutil.rmtree(TMPDIR, ignore_errors=True)

    sys.exit(0 if summary() else 1)
