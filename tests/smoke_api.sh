#!/usr/bin/env bash
# FluxDM — tests/smoke_api.sh
#
# Starts the REAL daemon (the Spring Boot jar) in a throw-away home directory and checks, over real HTTP and a
# real WebSocket, that the local API refuses what it must refuse and still serves what it must:
#
#   refuses   another Host (DNS rebinding), another Origin (cross-site requests, CORS preflights, the WebSocket
#             handshake), a missing or wrong token, STOMP frames from a session that never sent a valid CONNECT
#   serves    the right token, the UI's origin with CORS headers, preflights, the SockJS handshake, and a STOMP
#             session that did CONNECT with the token
#   keeps     the token file mode 0600, the runtime directory mode 0700, the core socket inside it
#
# This is the test for what tests/run_daemon_tests.sh cannot reach: how Spring itself wires the request filter,
# the STOMP interceptor and the CORS settings. The Java logic behind them is covered by ApiSecurityTest.
#
# Needs: java (21), curl, python3, the daemon jar (mvn -f daemon/pom.xml -DskipTests package) and the core
# (make -C core).
#
# Usage:  bash tests/smoke_api.sh [--jar PATH] [--core PATH] [--port N] [--keep]
#   --keep   leave the temporary directory (daemon log, database, token) in place; a failed run keeps it anyway
#
# Environment: SMOKE_START_TIMEOUT (seconds to wait for the daemon to answer, default 120) and TMPDIR (where the
# throw-away home goes; keep it short, because the core's socket path inside it may not exceed 107 bytes).
set -uo pipefail

GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"; BOLD="\033[1m"; RESET="\033[0m"
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JAR="$ROOT/daemon/target/fluxdm-daemon.jar"
CORE="$ROOT/core/fluxdm-core"
PORT=""
KEEP=0
START_TIMEOUT="${SMOKE_START_TIMEOUT:-120}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jar)      JAR="${2:?--jar needs a path}"; shift ;;
        --core)     CORE="${2:?--core needs a path}"; shift ;;
        --port)     PORT="${2:?--port needs a number}"; shift ;;
        --keep)     KEEP=1 ;;
        -h|--help)  sed -n '2,23p' "$0"; exit 0 ;;
        *)          echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

for tool in java curl python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo -e "  ${RED}✘${RESET}  $tool is required but was not found" >&2; exit 2; }
done
[[ -f "$JAR" ]]  || { echo -e "  ${RED}✘${RESET}  $JAR not found. Build it first:  mvn -f daemon/pom.xml -DskipTests package" >&2; exit 2; }
[[ -x "$CORE" ]] || { echo -e "  ${RED}✘${RESET}  $CORE not found or not executable. Build it first:  make -C core" >&2; exit 2; }

# Keep this path short: the daemon's socket goes under it, and a socket path may not exceed 107 bytes.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fluxdm-smoke.XXXXXX")" || { echo "cannot create a temporary directory" >&2; exit 2; }
WORK="$(cd "$WORK" && pwd -P)"
HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR"
if [[ -z "$PORT" ]]; then
    PORT="$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
fi
BASE="http://127.0.0.1:$PORT"
OK_ORIGIN="http://localhost:3000"
BAD_ORIGIN="http://evil.example"

PASS=0; FAIL=0
pass() { echo -e "  ${GREEN}✔${RESET}  $*"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}✘${RESET}  $*"; FAIL=$((FAIL + 1)); }
check() {                                 # check <what is true> <what was found instead> <command...>
    local good="$1" found="$2"; shift 2
    if "$@"; then pass "$good"; else fail "$good — $found"; fi
}
mode_of() { stat -c %a "$1" 2>/dev/null; }

DAEMON_PID=""; CORE_PID=""
# shellcheck disable=SC2329  # runs from the EXIT trap below
cleanup() {
    if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null
        for _ in $(seq 1 50); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.2; done
        kill -9 "$DAEMON_PID" 2>/dev/null
    fi
    # The daemon stops the core it spawned; this is only for a daemon that did not get the chance. The name is
    # checked first so that a process number that has since been reused is never signalled.
    if [[ -n "$CORE_PID" && "$(cat "/proc/$CORE_PID/comm" 2>/dev/null)" == "fluxdm-core" ]]; then
        kill "$CORE_PID" 2>/dev/null
    fi
    if [[ $KEEP -eq 1 ]]; then echo "  (kept $WORK)"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"
echo -e "${BOLD}  FluxDM — API smoke test (real daemon)${RESET}"
echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"

# ── Start the daemon ──────────────────────────────────────────────────────────
info "Starting the daemon on port $PORT (home: $HOME_DIR)"
# XDG_RUNTIME_DIR is removed so the daemon uses ~/.fluxdm/run inside the throw-away home.
env -u XDG_RUNTIME_DIR HOME="$HOME_DIR" \
    java -Duser.home="$HOME_DIR" -jar "$JAR" \
         --server.port="$PORT" \
         --fluxdm.core.binary-path="$CORE" \
         > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!

ready=0
for ((i = 0; i < START_TIMEOUT * 2; i++)); do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then break; fi
    code="$(curl -s --noproxy '*' -o /dev/null --max-time 2 -w '%{http_code}' "$BASE/api/health" 2>/dev/null || true)"
    if [[ -n "$code" && "$code" != "000" ]]; then ready=1; break; fi
    sleep 0.5
done
if [[ $ready -ne 1 ]]; then
    echo -e "  ${RED}✘${RESET}  the daemon did not start answering within ${START_TIMEOUT}s. Its log:" >&2
    tail -40 "$WORK/daemon.log" >&2
    exit 1
fi
pass "the daemon answers on $BASE"

TOKEN_FILE="$HOME_DIR/.fluxdm/api-token"
RUN_DIR="$HOME_DIR/.fluxdm/run"
SOCK="$RUN_DIR/core.sock"

# ── Files and directories ─────────────────────────────────────────────────────
info "Token file and runtime directory"
check "the token file exists" "there is no $TOKEN_FILE" test -f "$TOKEN_FILE"
TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE" 2>/dev/null || true)"
check "...with mode 0600" "its mode is $(mode_of "$TOKEN_FILE")" test "$(mode_of "$TOKEN_FILE")" = 600
if [[ "$TOKEN" =~ ^[A-Za-z0-9_-]{43}$ ]]; then                  # 256 random bits, base64url, no padding
    pass "...and 43 URL-safe characters (256 bits)"
else
    fail "...and 43 URL-safe characters (256 bits) — the token has ${#TOKEN} characters"
fi
check "the runtime directory $RUN_DIR has mode 0700" "its mode is $(mode_of "$RUN_DIR")" test "$(mode_of "$RUN_DIR")" = 700
check "the core's socket is inside it" "there is no socket at $SOCK (did the core start there?)" test -S "$SOCK"
check "...with mode 0600" "its mode is $(mode_of "$SOCK")" test "$(mode_of "$SOCK")" = 600
if [[ -z "$TOKEN" ]]; then
    fail "there is no token to test with, so nothing more can be checked"
    tail -20 "$WORK/daemon.log" >&2
    KEEP=1
    exit 1
fi
# A token that is certainly wrong: the real one with its last character changed.
if [[ "${TOKEN: -1}" == "A" ]]; then WRONG_TOKEN="${TOKEN:0:-1}B"; else WRONG_TOKEN="${TOKEN:0:-1}A"; fi

# ── HTTP ──────────────────────────────────────────────────────────────────────
STATUS=""
req() {                                   # req <curl args...>  →  $STATUS, $WORK/body, $WORK/headers
    : > "$WORK/body"; : > "$WORK/headers"
    STATUS="$(curl -s --noproxy '*' --max-time 10 -o "$WORK/body" -D "$WORK/headers" -w '%{http_code}' "$@" 2>/dev/null || true)"
    [[ -n "$STATUS" ]] || STATUS="000"
}
header() {                                # header <name>  →  the value from the last response, CR and blanks removed
    grep -i "^$1:" "$WORK/headers" | head -1 | cut -d: -f2- | tr -d '\r' | sed 's/^ *//; s/ *$//'
}
expect_status() {                         # expect_status <description> <status>...
    local what="$1"; shift
    for want in "$@"; do [[ "$STATUS" == "$want" ]] && { pass "$what ($STATUS)"; return; }; done
    fail "$what: expected $*, got $STATUS (body: $(head -c 200 "$WORK/body"))"
}
expect_body() {                           # expect_body <description> <text>
    if grep -qF -- "$2" "$WORK/body"; then pass "$1"; else fail "$1: the body lacks \"$2\" (body: $(head -c 200 "$WORK/body"))"; fi
}
expect_header() {                         # expect_header <description> <name> <substring, case-insensitive>
    local v; v="$(header "$2")"
    if [[ "${v,,}" == *"${3,,}"* ]]; then pass "$1"; else fail "$1: header $2 is \"$v\", expected it to contain \"$3\""; fi
}
expect_no_header() {                      # expect_no_header <description> <name>
    local v; v="$(header "$2")"
    if [[ -z "$v" ]]; then pass "$1"; else fail "$1: header $2 should be absent but is \"$v\""; fi
}

AUTH="Authorization: Bearer $TOKEN"

info "No token, wrong token, right token"
req "$BASE/api/health"
expect_status "a request without a token is refused" 401
expect_body   "...with a JSON error" '"error":"missing or invalid API token"'
expect_header "...naming the scheme the API wants" WWW-Authenticate "Bearer"
req -H "Authorization: Bearer $WRONG_TOKEN" "$BASE/api/health"
expect_status "a wrong token is refused" 401
req -H "Authorization: Basic $TOKEN" "$BASE/api/health"
expect_status "the token under another scheme is refused" 401
req -H "$AUTH" "$BASE/api/health"
expect_status "the right token is accepted" 200
expect_body   "...and the health answer reports the core's socket in the private directory" "\"socket\":\"$SOCK\""
CORE_PID="$(grep -o '"pid":[0-9]*' "$WORK/body" | head -1 | cut -d: -f2)"
req -H "$AUTH" -X DELETE "$BASE/api/downloads/999999"
case "$STATUS" in
    401|403|000) fail "an authenticated state-changing request reaches the controller — it was refused or failed ($STATUS)" ;;
    *)           pass "an authenticated state-changing request reaches the controller ($STATUS)" ;;
esac

info "Host: DNS rebinding"
req -H "$AUTH" -H "Host: evil.example:$PORT" "$BASE/api/health"
expect_status "another host name is refused even with the token" 403
expect_body   "...and the answer does not repeat what the client sent" '"error":"host not allowed"'
req -H "$AUTH" -H "Host: 127.0.0.1.evil.example" "$BASE/api/health"
expect_status "a host name that merely starts like a loopback address is refused" 403
for host in "localhost:$PORT" "127.0.0.1:$PORT" "[::1]:$PORT"; do
    req -H "$AUTH" -H "Host: $host" "$BASE/api/health"
    expect_status "Host: $host is accepted" 200
done

info "Origin: requests from web pages"
req -H "$AUTH" -H "Origin: $BAD_ORIGIN" "$BASE/api/health"
expect_status "another web origin is refused even with the token" 403
expect_body   "...with a fixed message" '"error":"origin not allowed"'
expect_no_header "...and without a CORS header, so the page cannot read even the refusal" Access-Control-Allow-Origin
req -X POST -H "Origin: $BAD_ORIGIN" -H 'Content-Type: text/plain' \
    --data '{"url":"http://127.0.0.1:9/x","filePath":"/tmp/fluxdm-smoke-evil"}' "$BASE/api/downloads"
expect_status "a simple cross-site POST (no preflight) is refused" 403
req -H "$AUTH" "$BASE/api/downloads"
expect_status "...and the download list can still be read with the token" 200
check "...and it is empty: the refused POST started nothing" "the list is $(head -c 200 "$WORK/body")" \
      test "$(tr -d ' \r\n' < "$WORK/body")" = "[]"
req -H "$AUTH" -H "Origin: null" "$BASE/api/health"
expect_status "Origin: null (sandboxed frames, data: pages) is refused" 403

req -H "$AUTH" -H "Origin: $OK_ORIGIN" "$BASE/api/health"
expect_status "the UI's origin is accepted with the token" 200
expect_header "...and its page may read the answer (CORS)" Access-Control-Allow-Origin "$OK_ORIGIN"
req -H "Origin: $OK_ORIGIN" "$BASE/api/health"
expect_status "the UI's origin without the token is refused" 401
expect_header "...and its page may read that refusal, to tell the user a token is needed" Access-Control-Allow-Origin "$OK_ORIGIN"

info "CORS preflight (a browser sends it before a request with Authorization or JSON)"
req -X OPTIONS -H "Origin: $OK_ORIGIN" -H 'Access-Control-Request-Method: POST' \
    -H 'Access-Control-Request-Headers: authorization,content-type' "$BASE/api/downloads"
expect_status "a preflight from the UI's origin is answered without a token" 200 204
expect_header "...allowing that origin" Access-Control-Allow-Origin "$OK_ORIGIN"
expect_header "...the POST method" Access-Control-Allow-Methods "POST"
expect_header "...and the Authorization header the token travels in" Access-Control-Allow-Headers "authorization"
req -X OPTIONS -H "Origin: $BAD_ORIGIN" -H 'Access-Control-Request-Method: POST' "$BASE/api/downloads"
expect_status "a preflight from another origin is refused" 403
req -X OPTIONS -H "Host: evil.example" -H "Origin: $OK_ORIGIN" -H 'Access-Control-Request-Method: POST' "$BASE/api/downloads"
expect_status "a preflight that names another host is refused" 403
req -X OPTIONS -H "Origin: $OK_ORIGIN" "$BASE/api/downloads"
expect_status "OPTIONS that is not a preflight needs the token" 401

info "WebSocket endpoint (SockJS) over HTTP"
req -H "Origin: $OK_ORIGIN" "$BASE/ws/info"
expect_status "/ws/info from the UI's origin is served (the token goes in the STOMP CONNECT frame)" 200
expect_body   "...and offers the WebSocket transport" '"websocket":true'
req -H "Origin: $BAD_ORIGIN" "$BASE/ws/info"
expect_status "/ws/info from another origin is refused" 403
req -H "Host: evil.example" "$BASE/ws/info"
expect_status "/ws/info with another host name is refused" 403
req --path-as-is -H "Origin: $OK_ORIGIN" "$BASE/ws/../api/downloads"
expect_status "a path that only pretends to be under /ws (dot segments) still needs the token" 401 400

# ── WebSocket + STOMP, spoken by hand ─────────────────────────────────────────
info "STOMP over WebSocket"
python3 -I - "$PORT" "$TOKEN" "$WRONG_TOKEN" <<'PY' > "$WORK/ws.out" 2>&1
import base64, os, socket, struct, sys, time

HOST, PORT, TOKEN, WRONG_TOKEN = "127.0.0.1", int(sys.argv[1]), sys.argv[2], sys.argv[3]
OK_ORIGIN, BAD_ORIGIN = "http://localhost:3000", "http://evil.example"


class Conn:
    def __init__(self, sock, buf=b""):
        self.sock, self.buf = sock, buf

    def need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError()
            self.sock.settimeout(left)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError()
            self.buf += chunk

    def frame(self, timeout):
        deadline = time.time() + timeout
        self.need(2, deadline)
        opcode, length, off = self.buf[0] & 0x0F, self.buf[1] & 0x7F, 2
        if length == 126:
            self.need(4, deadline); length, off = struct.unpack(">H", self.buf[2:4])[0], 4
        elif length == 127:
            self.need(10, deadline); length, off = struct.unpack(">Q", self.buf[2:10])[0], 10
        self.need(off + length, deadline)
        payload, self.buf = self.buf[off:off + length], self.buf[off + length:]
        return opcode, payload

    def send(self, text):
        data = text.encode()
        head = bytearray([0x81])
        if len(data) < 126:
            head.append(0x80 | len(data))
        elif len(data) < 65536:
            head.append(0x80 | 126); head += struct.pack(">H", len(data))
        else:
            head.append(0x80 | 127); head += struct.pack(">Q", len(data))
        mask = os.urandom(4)
        head += mask
        self.sock.sendall(bytes(head) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def next_stomp(self, timeout):
        """The next STOMP frame's command, or 'CLOSED', or None if nothing came within the timeout."""
        deadline = time.time() + timeout
        try:
            while True:
                opcode, payload = self.frame(max(0.05, deadline - time.time()))
                if opcode == 8:
                    return "CLOSED", ""
                if opcode in (1, 2):
                    text = payload.decode(errors="replace").lstrip("\n")
                    if text:
                        return text.split("\n", 1)[0], text
        except TimeoutError:
            return None, ""
        except (EOFError, ConnectionError):
            return "CLOSED", ""


def open_ws(origin=None, host=None):
    sock = socket.create_connection((HOST, PORT), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    lines = ["GET /ws/websocket HTTP/1.1", "Host: " + (host or f"{HOST}:{PORT}"), "Upgrade: websocket",
             "Connection: Upgrade", "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13",
             "Sec-WebSocket-Protocol: v12.stomp"]
    if origin:
        lines.append("Origin: " + origin)
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(4096)
        if not chunk:
            break
        head += chunk
    status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0
    rest = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""
    return Conn(sock, rest), status


def connect_frame(auth=None):
    return "CONNECT\naccept-version:1.2\nheart-beat:0,0\n" + (f"Authorization:{auth}\n" if auth else "") + "\n\x00"


SUBSCRIBE = "SUBSCRIBE\nid:sub-0\ndestination:/topic/downloads\n\n\x00"
SEND_STATUS = "SEND\ndestination:/app/status-all\n\n\x00"


def report(ok, text):
    print(("PASS " if ok else "FAIL ") + text, flush=True)


def refused(frame):
    return frame in ("ERROR", "CLOSED")


# 1. the handshake itself
conn, status = open_ws(BAD_ORIGIN)
report(status == 403, f"the WebSocket handshake from another origin is refused (HTTP {status})")
conn, status = open_ws(OK_ORIGIN, host="evil.example")
report(status == 403, f"the WebSocket handshake with another host name is refused (HTTP {status})")

# 2. CONNECT without a valid token
for label, auth in (("without an Authorization header", None), ("with a wrong token", "Bearer " + WRONG_TOKEN),
                    ("with the token under another scheme", "Basic " + TOKEN)):
    conn, status = open_ws(OK_ORIGIN)
    if status != 101:
        report(False, f"the handshake from the UI's origin should succeed (HTTP {status})")
        continue
    conn.send(connect_frame(auth))
    frame, _ = conn.next_stomp(5)
    report(refused(frame), f"CONNECT {label} is refused (got {frame})")

# 3. frames before any CONNECT
for name, text in (("SUBSCRIBE", SUBSCRIBE), ("SEND", SEND_STATUS)):
    conn, status = open_ws(OK_ORIGIN)
    conn.send(text)
    frame, _ = conn.next_stomp(4)
    report(refused(frame), f"{name} before CONNECT is refused (got {frame})")

# 4. CONNECT with the token, then use the session
conn, status = open_ws(OK_ORIGIN)
conn.send(connect_frame("Bearer " + TOKEN))
frame, _ = conn.next_stomp(8)
report(frame == "CONNECTED", f"CONNECT with the token is accepted (got {frame})")
if frame == "CONNECTED":
    conn.send(SUBSCRIBE)
    conn.send(SEND_STATUS)
    frame, text = conn.next_stomp(2)
    ok = frame is None or frame == "MESSAGE"
    report(ok, "SUBSCRIBE and SEND after a valid CONNECT are accepted: the session stays authenticated "
               f"(got {frame}{': ' + text.splitlines()[1] if frame == 'ERROR' and len(text.splitlines()) > 1 else ''})")

# 5. a client that sends no Origin at all (curl, a script) is fine with the token
conn, status = open_ws(None)
if status == 101:
    conn.send(connect_frame("Bearer " + TOKEN))
    frame, _ = conn.next_stomp(8)
    report(frame == "CONNECTED", f"a client that sends no Origin can CONNECT with the token (got {frame})")
else:
    report(False, f"a client that sends no Origin can open the WebSocket (HTTP {status})")
PY
WS_RC=$?
if [[ $WS_RC -ne 0 ]]; then
    fail "the WebSocket checks crashed (exit $WS_RC): $(tail -5 "$WORK/ws.out")"
fi
while IFS= read -r line; do
    case "$line" in
        "PASS "*) pass "${line#PASS }" ;;
        "FAIL "*) fail "${line#FAIL }" ;;
        *)        [[ -n "$line" ]] && echo "     $line" ;;
    esac
done < "$WORK/ws.out"

# ── The log ───────────────────────────────────────────────────────────────────
info "Daemon log"
check "the daemon logged that authentication is on and where the token is" "that log line is missing" \
      grep -q "API authentication is on" "$WORK/daemon.log"
if grep -qF -- "$TOKEN" "$WORK/daemon.log"; then fail "THE TOKEN IS IN THE DAEMON LOG"; else pass "the token is not in the daemon log"; fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo -e "\n${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"
if [[ $FAIL -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  API SMOKE TEST PASSED${RESET}"
    exit 0
fi
echo -e "${BOLD}${RED}  API SMOKE TEST FAILED${RESET}"
echo "  daemon log (tail):"
tail -25 "$WORK/daemon.log" | sed 's/^/     /'
KEEP=1
exit 1
