#!/usr/bin/env bash
# FluxDM — tests/run_all.sh
#
# Runs every test suite of the C core, in order of increasing cost:
#   1. unit tests        (errors, bandwidth, checksum, resume, URL / HTTP parsing, IPC framing)
#   2. integration smoke (a private fluxdm-core instance, the real IPC protocol)
#   3. behaviour suite   (retries, pause/resume, HTTPS, crash recovery, concurrency, IPC abuse ...)
#
# Options:
#   --quick     shorter behaviour suite (skips the big throughput run)
#   --no-core   run only the unit tests
#
# The core binary is core/fluxdm-core (build it with `make -C core`); set FLUXDM_CORE
# to test another build, e.g. an AddressSanitizer one.
set -uo pipefail

GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"; YELLOW="\033[93m"
BOLD="\033[1m"; RESET="\033[0m"
sep() { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

QUICK=""; WITH_CORE=1
for a in "$@"; do
    case "$a" in
        --quick)   QUICK="--quick" ;;
        --no-core) WITH_CORE=0 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

sep; echo -e "${BOLD}  FluxDM — Full Test Suite${RESET}"; sep

FAILED=()
run_step() {            # run_step "<title>" <command...>
    local title="$1"; shift
    echo -e "\n${BOLD}${CYAN}[ ${title} ]${RESET}"
    if "$@"; then
        echo -e "\n  ${GREEN}✔  ${title} PASSED${RESET}"
    else
        echo -e "\n  ${RED}✘  ${title} FAILED${RESET}"
        FAILED+=("$title")
    fi
}

if ! make -s unit unit-io; then
    echo -e "  ${RED}✘  could not build the unit tests${RESET}"; exit 1
fi
run_step "Unit tests"        ./test_unit
run_step "Unit tests (URL / HTTP / IPC framing)" ./test_unit_io

if [[ $WITH_CORE -eq 1 ]]; then
    CORE_BIN="${FLUXDM_CORE:-$SCRIPT_DIR/../core/fluxdm-core}"
    if [[ ! -x "$CORE_BIN" ]]; then
        echo -e "\n  ${YELLOW}⚠${RESET}  $CORE_BIN not found — building it"
        make -s -C "$SCRIPT_DIR/../core" || { echo -e "  ${RED}✘  core build failed${RESET}"; exit 1; }
    fi
    run_step "Integration tests"  python3 test_integration.py
    run_step "Core behaviour tests" python3 test_core.py $QUICK
fi

sep
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  ALL TESTS PASSED${RESET}"; sep; exit 0
else
    echo -e "${BOLD}${RED}  FAILED: ${FAILED[*]}${RESET}"; sep; exit 1
fi
