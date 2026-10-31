#!/usr/bin/env bash
# FluxDM — tests/run_daemon_tests.sh
#
# Tests for the Java daemon. They need only a JDK (21) — no Maven, no Spring
# jars, no internet:
#
#   ApiSecurityTest       the local API's defences: the private socket directory, the token
#                         file, the Host / Origin / token checks, the HTTP filter and the
#                         STOMP interceptor (driven through stand-in servlet and message objects)
#   QueueServiceTest      who may run: slots by id, priorities, idempotent release
#   CoreIPCClientTest     the wire protocol: replies vs events, ordering, timeouts,
#                         reconnects, protocol abuse
#   DownloadServiceTest   the download logic against a scripted fake core: validation,
#                         queueing, pause/resume/cancel, errors, recovery after a core
#                         or daemon restart, the scheduler, the bandwidth service
#   DaemonE2ETest         the same classes against the REAL fluxdm-core and a local HTTP
#                         server: queueing, pause/resume, cancel, checksums, kill -9 of
#                         the core, a daemon restart that adopts the running core, ...
#
# How the daemon compiles without Spring: tests/java/stubs holds minimal stand-ins
# for the few Spring / Jakarta / SLF4J types the daemon's source mentions
# (annotations, SimpMessagingTemplate, JpaRepository, ...). That type-checks every
# line of daemon/src/main and lets the real service classes run. It is NOT Spring:
# bean wiring, JPA/SQLite, the REST mappings and WebSocket are exercised only by a
# real build — the Maven step at the end does that when Maven can reach Maven Central.
#
# Options:
#   --no-e2e     skip the suite that needs the real core binary and takes ~40 s
#   --no-maven   skip the Maven compile
#   --loop N     run the four fast suites N times (to flush out timing flakiness)
#
# The core binary is core/fluxdm-core (built with `make -C core` if missing); set
# FLUXDM_CORE to test another build.
set -uo pipefail

GREEN="\033[92m"; RED="\033[91m"; YELLOW="\033[93m"; CYAN="\033[96m"
BOLD="\033[1m"; RESET="\033[0m"
ok()   { echo -e "  ${GREEN}✔${RESET}  $*"; }
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }
warn() { echo -e "  ${YELLOW}⚠${RESET}  $*"; }
err()  { echo -e "  ${RED}✘${RESET}  $*"; }
sep()  { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON="$ROOT/daemon"
JTESTS="$ROOT/tests/java"
BUILD="$ROOT/tests/.daemon_build"
OUT="$BUILD/classes"

E2E=1; MAVEN=1; LOOP=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-e2e)   E2E=0 ;;
        --no-maven) MAVEN=0 ;;
        --loop)     LOOP="${2:-}"; shift
                    [[ "$LOOP" =~ ^[1-9][0-9]*$ ]] || { echo "--loop needs a positive number" >&2; exit 2; } ;;
        -h|--help)  sed -n '2,33p' "$0"; exit 0 ;;
        *)          echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

sep
echo -e "${BOLD}  FluxDM — Daemon Tests${RESET}"
sep

# ── JDK ───────────────────────────────────────────────────────────────────────
command -v javac >/dev/null 2>&1 && command -v java >/dev/null 2>&1 \
    || { err "a JDK is required (javac / java not found). Run: sudo apt install openjdk-21-jdk-headless"; exit 1; }
JDK_MAJOR="$(javac -version 2>&1 | sed -nE 's/^javac ([0-9]+).*/\1/p' | head -1)"
if [[ -z "$JDK_MAJOR" || "$JDK_MAJOR" -lt 17 ]]; then
    err "JDK 17 or newer is required (found: $(javac -version 2>&1 | tail -1))"; exit 1
fi
[[ "$JDK_MAJOR" -lt 21 ]] && warn "the daemon targets Java 21; this is JDK $JDK_MAJOR"

# ── Compile: stubs + the daemon's main sources + the test harness ─────────────
info "Compiling the daemon and the test harness (JDK $JDK_MAJOR, no dependencies)"
rm -rf "$BUILD"; mkdir -p "$OUT"
find "$JTESTS/stubs" "$DAEMON/src/main/java" "$JTESTS/harness" -name '*.java' | sort | sed 's/.*/"&"/' > "$BUILD/sources.txt"
if javac -Xlint:all -encoding UTF-8 -d "$OUT" @"$BUILD/sources.txt" 2> "$BUILD/javac.log"; then
    ok "compiled $(wc -l < "$BUILD/sources.txt") files (every file of daemon/src/main type-checks)"
    if grep -v "^Picked up" "$BUILD/javac.log" | grep -q "warning"; then
        warn "the compiler had warnings:"; grep -v "^Picked up" "$BUILD/javac.log" | head -20
    fi
else
    err "compilation failed:"; grep -v "^Picked up" "$BUILD/javac.log"
    exit 1
fi

# ── Run the suites ────────────────────────────────────────────────────────────
JAVA_OPTS=(-Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 "-Dfluxdm.root=$ROOT")
SUMMARY=(); FAILED=()

run_suite() {                       # run_suite <Class> [args...]
    local name="$1"; shift
    local log="$BUILD/$name.log"
    info "$name"
    java "${JAVA_OPTS[@]}" -cp "$OUT" "harness.$name" "$@" 2>&1 | grep -v "^Picked up" | tee "$log"
    local rc=${PIPESTATUS[0]}
    local passed failed
    passed="$(grep -E '^ +Passed:' "$log" | tail -1 | awk '{print $2}')"
    failed="$(grep -E '^ +Failed:' "$log" | tail -1 | awk '{print $2}')"
    if [[ $rc -eq 0 ]]; then
        SUMMARY+=("${GREEN}✔${RESET}  $(printf '%-20s' "$name") ${passed:-?} checks passed")
    else
        SUMMARY+=("${RED}✘${RESET}  $(printf '%-20s' "$name") FAILED (${failed:-?} failed, exit code $rc) — log: $log")
        FAILED+=("$name")
    fi
    return $rc
}

for ((i = 1; i <= LOOP; i++)); do
    [[ $LOOP -gt 1 ]] && info "round $i of $LOOP"
    run_suite ApiSecurityTest
    run_suite QueueServiceTest
    run_suite CoreIPCClientTest
    run_suite DownloadServiceTest
done

if [[ $E2E -eq 1 ]]; then
    CORE_BIN="${FLUXDM_CORE:-$ROOT/core/fluxdm-core}"
    if [[ ! -x "$CORE_BIN" ]]; then
        warn "$CORE_BIN not found — building it"
        if ! make -s -C "$ROOT/core"; then
            err "the core did not build; cannot run the end-to-end suite"
            SUMMARY+=("${RED}✘${RESET}  $(printf '%-20s' DaemonE2ETest) not run: no fluxdm-core binary")
            FAILED+=("DaemonE2ETest")
            E2E=0
        fi
    fi
    [[ $E2E -eq 1 ]] && run_suite DaemonE2ETest "$CORE_BIN"
else
    SUMMARY+=("${YELLOW}⚠${RESET}  $(printf '%-20s' DaemonE2ETest) skipped (--no-e2e)")
fi

# ── Maven: the real Spring build (needs mvn and Maven Central) ────────────────
MVN_LINE=""
if [[ $MAVEN -eq 1 ]]; then
    info "Maven — compiling the daemon against the real Spring Boot libraries"
    if ! command -v mvn >/dev/null 2>&1; then
        warn "mvn not found — skipped (sudo apt install maven)"
        MVN_LINE="${YELLOW}⚠${RESET}  $(printf '%-20s' 'mvn compile') skipped (mvn not installed)"
    else
        ( cd "$DAEMON" && mvn -q -DskipTests compile ) > "$BUILD/maven.log" 2>&1
        MVN_RC=$?
        if [[ $MVN_RC -eq 0 ]]; then
            ok "mvn compile succeeded: the daemon builds against real Spring Boot"
            MVN_LINE="${GREEN}✔${RESET}  $(printf '%-20s' 'mvn compile') OK"
        elif grep -q "COMPILATION ERROR" "$BUILD/maven.log"; then
            err "mvn compile found compile errors:"; grep -E "ERROR.*\.java|COMPILATION ERROR" "$BUILD/maven.log" | head -30
            MVN_LINE="${RED}✘${RESET}  $(printf '%-20s' 'mvn compile') FAILED — log: $BUILD/maven.log"
            FAILED+=("mvn compile")
        else
            warn "mvn could not run to completion (usually no access to Maven Central) — see $BUILD/maven.log"
            MVN_LINE="${YELLOW}⚠${RESET}  $(printf '%-20s' 'mvn compile') not completed — log: $BUILD/maven.log"
        fi
    fi
fi

# ── Summary ───────────────────────────────────────────────────────────────────
sep
echo -e "${BOLD}  Summary${RESET}"
sep
for line in "${SUMMARY[@]}"; do echo -e "  $line"; done
[[ -n "$MVN_LINE" ]] && echo -e "  $MVN_LINE"
sep
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  ALL DAEMON TESTS PASSED${RESET}"; sep; exit 0
else
    echo -e "${BOLD}${RED}  FAILED: ${FAILED[*]}${RESET}"; sep; exit 1
fi
