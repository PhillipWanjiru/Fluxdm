#!/usr/bin/env bash
# FluxDM — tests/run_daemon_tests.sh
#
# Two independent checks:
#
#   1. Wire-protocol test (always runs, needs only a JDK — no Maven, no
#      Spring, no internet). Compiles CoreIPCClient.java + IPCEventListener
#      standalone (the single @Component annotation is stripped for this
#      compile only — DI wiring is Spring's job, not part of the protocol
#      logic being tested), starts mock_core.py — a Python stand-in that
#      speaks the exact byte-for-byte protocol validated against the real
#      fluxdm-core in Phase 1 — and runs the real Java client against it.
#
#   2. Full Maven build (only if `mvn` is installed and Maven Central is
#      reachable). Compiles every daemon file, including the Spring/JPA
#      code that the wire-protocol test above doesn't touch.
#
# Run this after any change to daemon/ to catch regressions in either layer.

set -euo pipefail

GREEN="\033[92m"; RED="\033[91m"; YELLOW="\033[93m"; CYAN="\033[96m"
BOLD="\033[1m"; RESET="\033[0m"
ok()   { echo -e "  ${GREEN}✔${RESET}  $*"; }
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }
warn() { echo -e "  ${YELLOW}⚠${RESET}  $*"; }
err()  { echo -e "  ${RED}✘${RESET}  $*"; }
sep()  { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON="$ROOT/daemon"
BUILD_DIR="$ROOT/tests/.daemon_build"

sep
echo -e "${BOLD}  FluxDM — Phase 3 Daemon Tests${RESET}"
sep

command -v javac >/dev/null 2>&1 || { err "javac not found. Run: sudo apt install openjdk-21-jdk-headless"; exit 1; }
command -v python3 >/dev/null 2>&1 || { err "python3 not found"; exit 1; }

# ── Part 1: wire-protocol test (no Maven needed) ──────────────────────────────
info "Part 1 — Wire protocol test (CoreIPCClient vs mock fluxdm-core)"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/src/com/fluxdm/ipc" "$BUILD_DIR/src/com/fluxdm/test" "$BUILD_DIR/out"

# Strip the single Spring annotation so this compiles with zero dependencies.
# Everything else in the file — the actual protocol logic being tested — is
# untouched pure-JDK code (java.net, java.nio, java.util.concurrent).
sed 's/import org.springframework.stereotype.Component;//; s/^@Component$//' \
    "$DAEMON/src/main/java/com/fluxdm/ipc/CoreIPCClient.java" \
    > "$BUILD_DIR/src/com/fluxdm/ipc/CoreIPCClient.java"
cp "$DAEMON/src/main/java/com/fluxdm/ipc/IPCEventListener.java" \
   "$BUILD_DIR/src/com/fluxdm/ipc/IPCEventListener.java"
cp "$DAEMON/src/test/java/com/fluxdm/test/TestCoreIPCClient.java" \
   "$BUILD_DIR/src/com/fluxdm/test/TestCoreIPCClient.java"

javac -d "$BUILD_DIR/out" \
    "$BUILD_DIR/src/com/fluxdm/ipc/CoreIPCClient.java" \
    "$BUILD_DIR/src/com/fluxdm/ipc/IPCEventListener.java" \
    "$BUILD_DIR/src/com/fluxdm/test/TestCoreIPCClient.java"
ok "Compiled CoreIPCClient + test harness"

rm -f /tmp/fluxdm_mock.sock
python3 "$ROOT/tests/mock_core.py" > "$BUILD_DIR/mock.log" 2>&1 &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true' EXIT

for _ in $(seq 1 30); do
    grep -q "READY" "$BUILD_DIR/mock.log" 2>/dev/null && break
    sleep 0.1
done

echo ""
( cd "$BUILD_DIR/out" && java com.fluxdm.test.TestCoreIPCClient )
PROTO_RC=$?

wait $MOCK_PID 2>/dev/null || true
trap - EXIT

echo ""
if [[ $PROTO_RC -eq 0 ]]; then
    ok "Wire protocol test PASSED"
else
    err "Wire protocol test FAILED — see output above"
fi

# ── Part 2: full Maven build (optional — needs internet + mvn) ───────────────
info "Part 2 — Full Maven build (Spring Boot, JPA, WebSocket)"

if ! command -v mvn >/dev/null 2>&1; then
    warn "mvn not found — skipping full build."
    warn "Install: sudo apt install maven"
    MAVEN_RC=2
else
    cd "$DAEMON"
    if mvn -q -DskipTests compile 2>&1 | tee "$BUILD_DIR/maven.log"; then
        ok "mvn compile succeeded — full daemon (Spring/JPA/WebSocket) builds clean"
        MAVEN_RC=0
    else
        err "mvn compile failed — see $BUILD_DIR/maven.log"
        warn "If this is a dependency-resolution error, check internet access to Maven Central"
        MAVEN_RC=1
    fi
fi

# ── Summary ────────────────────────────────────────────────────────────────
sep
echo -e "${BOLD}  Summary${RESET}"
sep
if [[ $PROTO_RC -eq 0 ]]; then
    echo -e "  ${GREEN}✔${RESET}  Wire protocol (CoreIPCClient)  — PASSED"
else
    echo -e "  ${RED}✘${RESET}  Wire protocol (CoreIPCClient)  — FAILED"
fi
case $MAVEN_RC in
    0) echo -e "  ${GREEN}✔${RESET}  Full Maven build               — PASSED" ;;
    1) echo -e "  ${RED}✘${RESET}  Full Maven build               — FAILED" ;;
    2) echo -e "  ${YELLOW}⚠${RESET}  Full Maven build               — SKIPPED (no mvn)" ;;
esac
sep

exit $PROTO_RC
