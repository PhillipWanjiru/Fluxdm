#!/usr/bin/env python3
"""FluxDM — tests/fluxtest.py

Shared helpers for the integration suites (test_integration.py, test_core.py):

  * the wire protocol (encode/decode, payload parsers),
  * Conn  — an IPC client that behaves like the Java daemon's CoreIPCClient:
            one persistent connection and one reader thread that files every
            frame as a reply (flags == 0) or an event (FLAG_EVENT), and that
            records any byte-level corruption instead of swallowing it,
  * Core  — starts / stops / kills a private fluxdm-core on its own socket, so a
            test run never touches a daemon you are actually using,
  * Site  — a scriptable HTTP(S) origin (ranges, keep-alive, chunked, redirects,
            auth, resets, throttling, gates ...),
  * a tiny reporting layer (info / expect / run_tests).
"""
from __future__ import annotations

import hashlib
import os
import queue
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

# ── Reporting ────────────────────────────────────────────────────────────────
GREEN, RED, YELLOW = "\033[92m", "\033[91m", "\033[93m"
CYAN, BOLD, DIM, RESET = "\033[96m", "\033[1m", "\033[2m", "\033[0m"
if not sys.stdout.isatty() and not os.environ.get("FORCE_COLOR"):
    GREEN = RED = YELLOW = CYAN = BOLD = DIM = RESET = ""

_counts = {"pass": 0, "fail": 0}
_failures: list[str] = []


def info(msg):
    print(f"\n{BOLD}{CYAN}── {msg}{RESET}", flush=True)


def note(msg):
    print(f"  {DIM}·  {msg}{RESET}", flush=True)


def warn(msg):
    print(f"  {YELLOW}⚠{RESET}  {msg}", flush=True)


def expect(cond, msg, detail=None):
    """Record one check. `detail` is printed only when the check fails."""
    if cond:
        _counts["pass"] += 1
        print(f"  {GREEN}✔{RESET}  {msg}", flush=True)
    else:
        _counts["fail"] += 1
        _failures.append(msg)
        print(f"  {RED}✘{RESET}  {msg}", flush=True)
        if detail:
            for line in str(detail).splitlines()[-12:]:
                print(f"       {DIM}{line}{RESET}", flush=True)
    return bool(cond)


def run_tests(title, tests, after=None):
    """Run test functions, turning an exception into a failure instead of an abort.
    `after` (optional) runs once all tests are done, before the summary is printed."""
    print(f"\n{BOLD}{CYAN} {title}{RESET}", flush=True)
    for fn in tests:
        t0 = time.time()
        try:
            fn()
        except Exception:                                   # noqa: BLE001 - report everything
            expect(False, f"{fn.__name__} raised an exception", traceback.format_exc())
        note(f"{fn.__name__}: {time.time() - t0:.1f}s")
    if after:
        try:
            after()
        except Exception:                                   # noqa: BLE001
            expect(False, "final checks raised an exception", traceback.format_exc())
    return summary(title)


def summary(title="Results"):
    total = _counts["pass"] + _counts["fail"]
    print(f"\n{BOLD}{CYAN}────────────────────────────────────────────────{RESET}")
    print(f"{BOLD}  {title}{RESET}")
    print(f"{CYAN}────────────────────────────────────────────────{RESET}")
    print(f"  {GREEN}✔  Passed : {_counts['pass']}{RESET}")
    print(f"  {RED if _counts['fail'] else DIM}✘  Failed : {_counts['fail']}{RESET}")
    print(f"     Total  : {total}")
    print(f"{CYAN}────────────────────────────────────────────────{RESET}")
    for f in _failures:
        print(f"  {RED}✘ {f}{RESET}")
    if _counts["fail"] == 0:
        print(f"  {BOLD}{GREEN}ALL TESTS PASSED{RESET}\n")
    else:
        print(f"  {BOLD}{RED}SOME TESTS FAILED{RESET}\n")
    return _counts["fail"] == 0


# ── Wire protocol ────────────────────────────────────────────────────────────
MAGIC, VER = 0x464C5558, 1
FLAG_EVENT = 0x0001
CMD_START, CMD_PAUSE, CMD_RESUME, CMD_CANCEL = 0x01, 0x02, 0x03, 0x04
CMD_SET_LIMIT, CMD_STATUS, CMD_STATUS_ALL, CMD_SHUTDOWN = 0x05, 0x06, 0x07, 0xFF
RSP_OK, RSP_ERROR, RSP_PROGRESS, RSP_COMPLETE, RSP_STATUS = 0x10, 0x11, 0x12, 0x13, 0x14
HDR_FMT = "<IBBHII"
HDR_SIZE = struct.calcsize(HDR_FMT)
MAX_PAYLOAD = 1 << 20

ERR = dict(NONE=0, INVALID_URL=1, NETWORK=2, TIMEOUT=3, DISK=4, TLS=5, NOT_FOUND=6,
           FORBIDDEN=7, SERVER=8, RANGE=9, CHECKSUM=10, INTERNAL=99)
ST = dict(IDLE=0, CONNECTING=1, DOWNLOADING=2, PAUSED=3, COMPLETE=4, ERROR=5, CANCELLED=6)
CK_NONE, CK_MD5, CK_SHA256 = 0, 1, 2
NAMES = {0x10: "OK", 0x11: "ERROR", 0x12: "PROGRESS", 0x13: "COMPLETE", 0x14: "STATUS"}


def encode_frame(typ, dl_id, payload=b"", flags=0):
    return struct.pack(HDR_FMT, MAGIC, VER, typ, flags, dl_id, len(payload)) + payload


def build_start(dl_id, url, dest, segments=8, bw_limit=0, cksum_type=CK_NONE, cksum=b""):
    body = struct.pack("<IHHQB32s7s", dl_id, segments, 0, bw_limit, cksum_type,
                       (bytes(cksum) + b"\0" * 32)[:32], b"\0" * 7)
    u, p = url.encode(), str(dest).encode()
    return body + struct.pack("<H", len(u)) + u + struct.pack("<H", len(p)) + p


def parse_error(payload):
    code = payload[0] if payload else 0
    msg = payload[4:].split(b"\0")[0].decode(errors="replace") if len(payload) > 4 else ""
    return code, msg


def parse_progress(p):
    i, total, done, speed, eta, act, status, _ = struct.unpack("<IQQIIBB2s", p)
    return dict(id=i, total=total, done=done, speed=speed, eta=eta, active=act, status=status)


def parse_status(p):
    i, status, act, tot_seg, _, total, done, speed, eta = struct.unpack("<IBBBBQQII", p)
    return dict(id=i, status=status, active=act, segments=tot_seg, total=total,
                done=done, speed=speed, eta=eta)


def parse_complete(p):
    i, total, ms, ck_ok, ck, _ = struct.unpack("<IQIB32s3s", p)
    return dict(id=i, total=total, elapsed_ms=ms, checksum_ok=ck_ok, checksum=ck)


# ── IPC client (mirrors the Java reader thread) ──────────────────────────────
class _Dl:
    def __init__(self):
        self.progress = None            # latest parsed RSP_PROGRESS
        self.nprogress = 0
        self.backwards = 0              # progress frames whose `done` went down
        self.over_total = 0             # progress frames with done > total (known total)
        self.terminal = queue.Queue()   # (type, parsed) of COMPLETE / async ERROR events
        self.max_done = 0
        self.first_reply_idx = None     # frame number of the first reply (flags == 0) for this id
        self.first_event_idx = None     # frame number of the first event for this id


class Conn:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.wlock = threading.Lock()
        self.rlock = threading.Lock()
        self.req_lock = threading.Lock()            # STATUS_ALL (owns the collector)
        self.id_locks: dict[int, threading.Lock] = {}
        self.replies: dict[int, queue.Queue] = {}
        self.dls: dict[int, _Dl] = {}
        self._collect = None            # list while a STATUS_ALL is in flight
        self.proto_errors: list[str] = []
        self.frames_seen = 0
        self.unflagged_events = 0       # PROGRESS/COMPLETE frames lacking FLAG_EVENT
        self.flagged_replies = 0        # OK/STATUS frames carrying FLAG_EVENT
        self.closed = threading.Event()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    # -- reading
    def _recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return bytes(buf)

    def dl(self, dl_id):
        with self.rlock:
            return self.dls.setdefault(dl_id, _Dl())

    def _reply_q(self, dl_id):
        with self.rlock:
            return self.replies.setdefault(dl_id, queue.Queue())

    def _read_loop(self):
        try:
            while True:
                hdr = self._recv_exact(HDR_SIZE)
                magic, ver, typ, flags, dl_id, plen = struct.unpack(HDR_FMT, hdr)
                if magic != MAGIC or ver != VER or plen > MAX_PAYLOAD or typ not in NAMES:
                    self.proto_errors.append(
                        f"bad frame after {self.frames_seen} good frames: magic={magic:#x} "
                        f"ver={ver} type={typ:#x} flags={flags:#x} id={dl_id} len={plen}")
                    return
                payload = self._recv_exact(plen) if plen else b""
                self.frames_seen += 1
                is_event = bool(flags & FLAG_EVENT)
                dl = self.dl(dl_id)
                if is_event and dl.first_event_idx is None:
                    dl.first_event_idx = self.frames_seen
                if not is_event and dl.first_reply_idx is None:
                    dl.first_reply_idx = self.frames_seen
                if typ in (RSP_PROGRESS, RSP_COMPLETE) and not is_event:
                    self.unflagged_events += 1
                if typ in (RSP_OK, RSP_STATUS) and is_event:
                    self.flagged_replies += 1
                if is_event:
                    self._on_event(typ, dl_id, payload)
                elif typ == RSP_STATUS and self._collect is not None:
                    self._collect.append(parse_status(payload))
                else:
                    self._reply_q(dl_id).put((typ, payload))
        except (EOFError, OSError):
            pass
        finally:
            self.closed.set()

    def _on_event(self, typ, dl_id, payload):
        d = self.dl(dl_id)
        if typ == RSP_PROGRESS:
            p = parse_progress(payload)
            if p["done"] < d.max_done:
                d.backwards += 1
            if p["total"] and p["done"] > p["total"]:
                d.over_total += 1
            d.max_done = max(d.max_done, p["done"])
            d.progress = p
            d.nprogress += 1
        elif typ == RSP_COMPLETE:
            d.terminal.put((RSP_COMPLETE, parse_complete(payload)))
        elif typ == RSP_ERROR:
            code, msg = parse_error(payload)
            d.terminal.put((RSP_ERROR, dict(code=code, msg=msg)))

    # -- writing
    def send(self, typ, dl_id, payload=b"", flags=0):
        with self.wlock:
            self.sock.sendall(encode_frame(typ, dl_id, payload, flags))

    def send_raw(self, data):
        with self.wlock:
            self.sock.sendall(data)

    def _await(self, q, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                return q.get(timeout=0.1)
            except queue.Empty:
                if self.closed.is_set():
                    try:
                        return q.get_nowait()
                    except queue.Empty:
                        return None, None
        return None, None

    def _id_lock(self, dl_id):
        with self.rlock:
            return self.id_locks.setdefault(dl_id, threading.Lock())

    def request(self, typ, dl_id, payload=b"", timeout=15.0):
        """Send a command, return its reply as (type, payload) — (None, None) on timeout.
        Requests for different ids may run concurrently from different threads."""
        with self._id_lock(dl_id):
            q = self._reply_q(dl_id)
            while not q.empty():
                q.get_nowait()
            try:
                self.send(typ, dl_id, payload)
            except OSError:
                return None, None
            return self._await(q, timeout)

    # -- friendly commands
    def start(self, dl_id, url, dest, **kw):
        """START; returns (reply_type, payload). Normally RSP_OK, then events follow."""
        return self.request(CMD_START, dl_id, build_start(dl_id, url, dest, **kw))

    def start_ok(self, dl_id, url, dest, **kw):
        typ, pl = self.start(dl_id, url, dest, **kw)
        return typ == RSP_OK

    def command(self, typ, dl_id, payload=b""):
        """Returns True for RSP_OK, else (code, msg) of the RSP_ERROR, else None."""
        rt, pl = self.request(typ, dl_id, payload)
        if rt == RSP_OK:
            return True
        if rt == RSP_ERROR:
            return parse_error(pl)
        return None

    def pause(self, dl_id):
        return self.command(CMD_PAUSE, dl_id)

    def resume(self, dl_id):
        return self.command(CMD_RESUME, dl_id)

    def cancel(self, dl_id):
        return self.command(CMD_CANCEL, dl_id)

    def set_limit(self, dl_id, bps):
        return self.command(CMD_SET_LIMIT, dl_id, struct.pack("<Q", bps))

    def status(self, dl_id):
        """Parsed RSP_STATUS dict, or the (code, msg) of an RSP_ERROR, or None."""
        rt, pl = self.request(CMD_STATUS, dl_id)
        if rt == RSP_STATUS:
            return parse_status(pl)
        if rt == RSP_ERROR:
            return parse_error(pl)
        return None

    def status_all(self, timeout=10.0):
        """CMD_STATUS_ALL -> list of parsed STATUS dicts (None if no closing OK)."""
        with self.req_lock:
            q = self._reply_q(0)
            while not q.empty():
                q.get_nowait()
            self._collect = []
            try:
                self.send(CMD_STATUS_ALL, 0)
                typ, _ = self._await(q, timeout)
            except OSError:
                typ = None
            got, self._collect = self._collect, None
            return got if typ == RSP_OK else None

    # -- events
    def wait_terminal(self, dl_id, timeout=60.0):
        """Block for the next COMPLETE / async ERROR of a download.
        Returns ("complete", dict) | ("error", dict(code,msg)) | None on timeout."""
        d = self.dl(dl_id)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                typ, val = d.terminal.get(timeout=0.1)
                return ("complete" if typ == RSP_COMPLETE else "error"), val
            except queue.Empty:
                if self.closed.is_set():
                    try:
                        typ, val = d.terminal.get_nowait()
                        return ("complete" if typ == RSP_COMPLETE else "error"), val
                    except queue.Empty:
                        return None
        return None

    def wait_done(self, dl_id, min_bytes, timeout=30.0):
        """Wait until a progress frame reports at least `min_bytes` downloaded."""
        d = self.dl(dl_id)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if d.max_done >= min_bytes:
                return True
            if self.closed.is_set():
                return False
            time.sleep(0.02)
        return False

    def wait_status(self, dl_id, want, timeout=15.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.status(dl_id)
            if isinstance(last, dict) and last["status"] == want:
                return last
            time.sleep(0.05)
        return last

    def close(self):
        """Close for real. A bare close() would NOT disconnect: the reader thread is
        blocked in recv() and keeps the kernel socket alive, so the core would never
        see EOF. shutdown() wakes it and sends the FIN first."""
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# ── Core process ─────────────────────────────────────────────────────────────
def find_core():
    env = os.environ.get("FLUXDM_CORE")
    if env:
        return Path(env)
    return Path(__file__).resolve().parent.parent / "core" / "fluxdm-core"


class Core:
    """A private fluxdm-core instance on its own socket."""

    def __init__(self, binary=None, args=(), env=None, log_level=1, preload=None):
        self.binary = str(binary or find_core())
        self.dir = Path(tempfile.mkdtemp(prefix="fx", dir="/tmp"))     # short: sun_path is 108 bytes
        self.sock_path = str(self.dir / "core.sock")
        self.log_path = self.dir / "core.log"
        self.args = list(args)
        self.log_level = log_level
        self.env = dict(os.environ)
        if env:
            self.env.update(env)
        pre = preload or os.environ.get("FLUXTEST_PRELOAD")
        if pre:
            self.env["LD_PRELOAD"] = pre
        self.proc = None
        self.log = None
        self.conns: list[Conn] = []

    def start(self, clean=True):
        """Launch the core. clean=False leaves a stale socket file in place, to
        check that the core replaces it by itself."""
        if clean:
            try:
                os.unlink(self.sock_path)
            except FileNotFoundError:
                pass
        self.log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [self.binary, "--sock", self.sock_path, "--log", str(self.log_level)] + self.args,
            stdout=self.log, stderr=self.log, env=self.env)
        for _ in range(200):
            if self.proc.poll() is not None:
                return False
            if os.path.exists(self.sock_path):
                try:
                    probe = Conn(self.sock_path)
                    probe.close()
                    return True
                except OSError:
                    pass
            time.sleep(0.05)
        return False

    def connect(self):
        c = Conn(self.sock_path)
        self.conns.append(c)
        return c

    @property
    def pid(self):
        return self.proc.pid

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def fd_count(self):
        try:
            return len(os.listdir(f"/proc/{self.proc.pid}/fd"))
        except OSError:
            return -1

    def thread_count(self):
        try:
            for line in Path(f"/proc/{self.proc.pid}/status").read_text().splitlines():
                if line.startswith("Threads:"):
                    return int(line.split()[1])
        except OSError:
            pass
        return -1

    def rss_kb(self):
        try:
            for line in Path(f"/proc/{self.proc.pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        except OSError:
            pass
        return -1

    def kill(self):
        """SIGKILL — simulates a crash / power loss."""
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        for c in self.conns:
            c.close()
        self.conns.clear()

    def stop(self, via="command", timeout=8.0):
        """Stop the core; returns its exit code (None if it had to be killed)."""
        if self.proc is None:
            return None
        rc = None
        if self.proc.poll() is None:
            delivered = False
            if via == "command":
                try:
                    c = Conn(self.sock_path)
                    c.send(CMD_SHUTDOWN, 0)
                    time.sleep(0.1)
                    c.close()
                    delivered = True
                except OSError:
                    pass
            if not delivered:
                self.proc.send_signal(signal.SIGTERM)
            try:
                rc = self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                rc = None
        else:
            rc = self.proc.returncode
        for c in self.conns:
            c.close()
        self.conns.clear()
        if self.log:
            self.log.close()
            self.log = None
        return rc

    def log_text(self):
        try:
            return self.log_path.read_text(errors="replace")
        except OSError:
            return ""

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


# ── Files ────────────────────────────────────────────────────────────────────
def make_file(root, name, size):
    """Random content (a repeating pattern would hide mis-placed segments).
    Returns the SHA-256 hex digest."""
    h = hashlib.sha256()
    path = Path(root) / name
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        left = size
        while left > 0:
            blk = os.urandom(min(left, 1 << 20))
            f.write(blk)
            h.update(blk)
            left -= len(blk)
    return h.hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.digest()


def tmpdir(prefix="fluxtest_"):
    return Path(tempfile.mkdtemp(prefix=prefix, dir="/tmp"))


def free_closed_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ── Scriptable HTTP origin ───────────────────────────────────────────────────
class _HTTPD(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 512
    allow_reuse_address = True

    def handle_error(self, request, client_address):          # resets are the point of some tests
        pass


class Site:
    """An HTTP(S) origin on 127.0.0.1 serving files from `root`.

    Behaviour is per path ("/name") through set(path, **opts), with defaults
    from the constructor. Options:
      ranges      honour Range (206) — False: always 200 with the whole body
      keepalive   HTTP/1.1 persistent connections
      head        "ok" | "drop" (close without answering) | "405"
      chunk,delay sleep `delay` seconds after every `chunk` bytes (throttle)
      chunked     answer GET with Transfer-Encoding: chunked (no Content-Length)
      no_length   answer GET 200 without Content-Length, ending at EOF
      auth        (user, password): demand HTTP Basic
      redirect    (code, location) instead of the file
      status      (code, retry_after|None) instead of the file ...
      status_times ... for the first K requests only (default: always)
      status_after ... skipping the first K requests (1 = let the probe through)
      script      one entry per request to this path, in the order the requests arrive (the
                  probe is #0): None = serve normally, (code, retry_after|None) = answer with
                  that status, ("reset", N) = reset the connection after N body bytes. Requests
                  past the end of the list are served normally.
      max_active  serve at most N GETs at the same time; the rest get `limit_status`
                  ((code, retry_after|None), default (429, None)) — a connection cap.
                  The Site counts them: refused, peak_active.
      reset_after reset the connection after N body bytes ...
      reset_times ... on the first K matching requests (default 1)
      hold_after  stall the body after N bytes until site.release is set
      etag        override ETag ("" = send none); default derives from size+mtime
      last_modified  True/False (default True)
    """

    DEFAULTS = dict(ranges=True, keepalive=False, head="ok", chunk=65536, delay=0.0,
                    chunked=False, no_length=False, auth=None, redirect=None, status=None,
                    status_times=None, status_after=0, reset_after=None, reset_times=1,
                    hold_after=None, etag=None, last_modified=True,
                    max_active=None, limit_status=(429, None), script=None)

    def __init__(self, root, tls=None, **defaults):
        self.root = Path(root)
        self.defaults = dict(self.DEFAULTS, **defaults)
        self.routes: dict[str, dict] = {}
        self.log: list[list] = []                  # [method, path, range, status, bytes sent, start time, user agent]
        self.status_done: dict[str, int] = {}
        self.seen: dict[str, int] = {}             # requests per path while a `status` rule is set
        self.script_pos: dict[str, int] = {}       # requests per path while a `script` is set
        self.active_gets = 0                       # GETs being served right now (max_active)
        self.peak_active = 0
        self.refused = 0                           # GETs turned away by max_active
        self.conn_count = 0
        self.resets_done: dict[str, int] = {}
        self.release = threading.Event()
        self.lock = threading.Lock()
        self.httpd = _HTTPD(("127.0.0.1", 0), self._make_handler())
        self.tls = bool(tls)
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(*tls)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True,
                                                do_handshake_on_connect=False)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    # -- configuration
    def set(self, path, **opts):
        self.routes[path] = opts

    def url(self, path, host="127.0.0.1", userinfo=None):
        scheme = "https" if self.tls else "http"
        auth = f"{userinfo}@" if userinfo else ""
        return f"{scheme}://{auth}{host}:{self.port}{path}"

    def opts(self, path):
        o = dict(self.defaults)
        o.update(self.routes.get(path, {}))
        return o

    def hold(self, path, after=64 * 1024, **opts):
        """Stall every response for `path` after `after` body bytes until release is set."""
        self.release.clear()
        self.set(path, hold_after=after, **opts)

    def sent_bytes(self, path=None, since=0):
        """Body bytes this server wrote for GETs (log entries from index `since`)."""
        with self.lock:
            return sum(r[4] for r in self.log[since:]
                       if r[0] == "GET" and (path is None or r[1] == path))

    def log_len(self):
        with self.lock:
            return len(self.log)

    def requests(self, path=None, method=None):
        with self.lock:
            return [r for r in self.log
                    if (path is None or r[1] == path) and (method is None or r[0] == method)]

    def stop(self):
        self.release.set()
        self.httpd.shutdown()
        self.httpd.server_close()

    # -- the handler
    def _make_handler(site):                                   # noqa: N805 - closure over the Site
        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def setup(self):
                super().setup()
                with site.lock:
                    site.conn_count += 1

            def _begin(self, path):
                """Log entries are created when a request STARTS, so their order is the
                order requests arrived in, and a snapshot of log_len() separates 'before'
                from 'after' even for requests that are still being served."""
                with site.lock:
                    self._entry = [self.command, path, self.headers.get("Range"), "pending", 0,
                                   time.monotonic(), self.headers.get("User-Agent")]
                    site.log.append(self._entry)

            def _record(self, status, sent=0):
                with site.lock:
                    self._entry[3] = status
                    self._entry[4] = sent

            def _finish(self, o):
                if not o["keepalive"]:
                    self.close_connection = True

            def _reply(self, code, headers, o, body=b""):
                self.send_response(code)
                for k, v in headers:
                    self.send_header(k, v)
                self.send_header("Connection", "keep-alive" if o["keepalive"] else "close")
                self.end_headers()
                if body and self.command != "HEAD":
                    self.wfile.write(body)
                self._finish(o)
                self._record(code)

            def do_HEAD(self):
                self._serve()

            def do_GET(self):
                path = unquote(self.path.split("?")[0])
                o = site.opts(path)
                cap = o["max_active"]
                if not cap:
                    self._serve()
                    return
                with site.lock:                                  # a connection cap, like a real server's
                    over = site.active_gets >= cap
                    if over:
                        site.refused += 1
                    else:
                        site.active_gets += 1
                        site.peak_active = max(site.peak_active, site.active_gets)
                if over:
                    self._begin(path)
                    code, retry_after = o["limit_status"]
                    hdrs = [("Content-Length", "0")]
                    if retry_after is not None:
                        hdrs.append(("Retry-After", str(retry_after)))
                    self._reply(code, hdrs, o)
                    return
                try:
                    self._serve()
                finally:
                    with site.lock:
                        site.active_gets -= 1

            def _serve(self):
                path = unquote(self.path.split("?")[0])
                self._begin(path)
                o = site.opts(path)

                if o["auth"]:
                    import base64
                    want = "Basic " + base64.b64encode(f"{o['auth'][0]}:{o['auth'][1]}".encode()).decode()
                    if self.headers.get("Authorization") != want:
                        self._reply(401, [("WWW-Authenticate", 'Basic realm="t"'),
                                          ("Content-Length", "0")], o)
                        return
                if o["redirect"]:
                    code, loc = o["redirect"]
                    self._reply(code, [("Location", loc), ("Content-Length", "0")], o)
                    return
                step = None
                if o["script"]:
                    with site.lock:
                        i = site.script_pos.get(path, 0)
                        site.script_pos[path] = i + 1
                    step = o["script"][i] if i < len(o["script"]) else None
                    if step is not None and step[0] != "reset":
                        code, retry_after = step
                        hdrs = [("Content-Length", "0")]
                        if retry_after is not None:
                            hdrs.append(("Retry-After", str(retry_after)))
                        self._reply(code, hdrs, o)
                        return

                if o["status"]:
                    with site.lock:
                        seen = site.seen.get(path, 0)
                        site.seen[path] = seen + 1
                        n = site.status_done.get(path, 0)
                        active = seen >= o["status_after"] and (o["status_times"] is None or n < o["status_times"])
                        if active:
                            site.status_done[path] = n + 1
                    if active:
                        code, retry_after = o["status"]
                        hdrs = [("Content-Length", "0")]
                        if retry_after is not None:
                            hdrs.append(("Retry-After", str(retry_after)))
                        self._reply(code, hdrs, o)
                        return

                p = site.root / path.lstrip("/")
                if not p.is_file():
                    self._reply(404, [("Content-Length", "0")], o)
                    return
                st = p.stat()
                size = st.st_size

                common = []
                if o["ranges"]:
                    common.append(("Accept-Ranges", "bytes"))
                etag = o["etag"]
                if etag is None:
                    etag = f'"{size:x}-{st.st_mtime_ns:x}"'
                if etag:
                    common.append(("ETag", etag))
                if o["last_modified"]:
                    common.append(("Last-Modified",
                                   self.date_time_string(st.st_mtime)))

                if self.command == "HEAD":
                    if o["head"] == "drop":
                        self.close_connection = True
                        self._record("drop")
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        return
                    if o["head"] == "405":
                        self._reply(405, [("Content-Length", "0")], o)
                        return
                    self._reply(200, common + [("Content-Length", str(size))], o)
                    return

                rng = self.headers.get("Range")
                start, end, code = 0, size - 1, 200
                if o["ranges"] and rng and rng.startswith("bytes="):
                    a, _, b = rng[6:].partition("-")
                    try:
                        if a == "":                                  # suffix range
                            start, end = max(0, size - int(b)), size - 1
                        else:
                            start = int(a)
                            end = min(int(b), size - 1) if b else size - 1
                    except ValueError:
                        start, end = 0, size - 1
                    if start >= size or start > end:
                        self._reply(416, [("Content-Range", f"bytes */{size}"),
                                          ("Content-Length", "0")], o)
                        return
                    code = 206
                length = end - start + 1

                self.send_response(code)
                for k, v in common:
                    self.send_header(k, v)
                if code == 206:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                if o["chunked"]:
                    self.send_header("Transfer-Encoding", "chunked")
                elif o["no_length"]:
                    pass
                else:
                    self.send_header("Content-Length", str(length))
                persistent = o["keepalive"] and not o["no_length"]
                self.send_header("Connection", "keep-alive" if persistent else "close")
                self.end_headers()
                if not persistent:
                    self.close_connection = True

                reset_at = o["reset_after"]            # None = never
                forced_reset = step is not None and step[0] == "reset"
                if forced_reset:
                    reset_at = step[1]

                sent = 0
                try:
                    with open(p, "rb") as f:
                        f.seek(start)
                        left = length
                        csize = 0
                        while left > 0:
                            blk = f.read(min(o["chunk"], left))
                            if not blk:
                                break
                            if reset_at is not None and sent + len(blk) > reset_at:
                                with site.lock:
                                    n = site.resets_done.get(path, 0)
                                    do_reset = forced_reset or n < o["reset_times"]
                                    if do_reset and not forced_reset:
                                        site.resets_done[path] = n + 1
                                if do_reset:
                                    blk = blk[:max(0, reset_at - sent)]
                                    if blk:
                                        self.wfile.write(blk)
                                        sent += len(blk)
                                    self._reset()
                                    self._record(f"{code}-reset", sent)
                                    return
                                reset_at = None
                            if o["chunked"]:
                                # awkward sizes + a chunk extension, to exercise the decoder
                                csize = (csize % 3) + 1
                                for part in _split(blk, csize):
                                    self.wfile.write(b"%x;x=1\r\n" % len(part) + part + b"\r\n")
                            else:
                                self.wfile.write(blk)
                            left -= len(blk)
                            sent += len(blk)
                            if o["hold_after"] is not None and sent >= o["hold_after"]:
                                site.release.wait(60)
                            if o["delay"]:
                                time.sleep(o["delay"])
                        if o["chunked"]:
                            self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n")
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError, OSError):
                    self.close_connection = True
                    self._record(f"{code}-aborted", sent)
                    return
                self._record(code, sent)

            def _reset(self):
                try:
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                               struct.pack("ii", 1, 0))
                    self.connection.close()
                except OSError:
                    pass
                self.close_connection = True

        return H


def _split(blk, n):
    """Cut `blk` into pieces of n different odd-ish sizes."""
    sizes = {1: 1000, 2: 4096, 3: 7000}
    step, out, i = sizes.get(n, 4096), [], 0
    while i < len(blk):
        out.append(blk[i:i + step])
        i += step
    return out


class DropServer:
    """Accepts connections and closes them without answering."""

    def __init__(self):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(64)
        self.port = self.srv.getsockname()[1]
        self.accepted = 0
        self.stop_flag = False
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while not self.stop_flag:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.accepted += 1
            try:
                c.recv(4096)
            except OSError:
                pass
            c.close()

    def stop(self):
        self.stop_flag = True
        self.srv.close()


class SilentServer:
    """Accepts connections and never says a word (tests the idle/read timeouts)."""

    def __init__(self):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(64)
        self.port = self.srv.getsockname()[1]
        self.conns = []
        self.stop_flag = False
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while not self.stop_flag:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.conns.append(c)

    def stop(self):
        self.stop_flag = True
        self.srv.close()
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass


# ── Local CA for the HTTPS tests ─────────────────────────────────────────────
def make_ca_and_cert(d, names=("localhost",), ips=("127.0.0.1",), cn="localhost"):
    """Create a throw-away CA and a server certificate with the given SANs.
    Returns (ca_pem, cert_pem, key_pem) paths, or None if openssl is missing."""
    if shutil.which("openssl") is None:
        return None
    d = Path(d)
    d.mkdir(parents=True, exist_ok=True)
    san = ",".join([f"DNS:{n}" for n in names] + [f"IP:{i}" for i in ips])
    (d / "ext.cnf").write_text(f"subjectAltName={san}\nbasicConstraints=CA:FALSE\n"
                               "keyUsage=digitalSignature,keyEncipherment\n"
                               "extendedKeyUsage=serverAuth\n")

    def run(*args):
        r = subprocess.run(args, cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{' '.join(args)}\n{r.stderr}")

    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
        "-out", "ca.pem", "-subj", "/CN=FluxTest-CA", "-days", "3",
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key",
        "-out", "srv.csr", "-subj", f"/CN={cn}")
    run("openssl", "x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
        "-CAcreateserial", "-out", "srv.pem", "-days", "3", "-extfile", "ext.cnf")
    return d / "ca.pem", d / "srv.pem", d / "srv.key"
