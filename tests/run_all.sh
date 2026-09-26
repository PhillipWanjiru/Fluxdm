#!/usr/bin/env bash
# FluxDM — tests/run_all.sh
set -euo pipefail
GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"
BOLD="\033[1m"; RESET="\033[0m"
sep() { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
sep; echo -e "${BOLD}  FluxDM — Full Test Suite${RESET}"; sep
UNIT_FAILED=0; INT_FAILED=0
echo -e "\n${BOLD}${CYAN}[ Unit Tests ]${RESET}"
make -s unit
if ./test_unit; then
    echo -e "\n  ${GREEN}✔  Unit tests PASSED${RESET}"
else
    echo -e "\n  ${RED}✘  Unit tests FAILED${RESET}"; UNIT_FAILED=1
fi
echo -e "\n${BOLD}${CYAN}[ Integration Tests ]${RESET}"
if python3 test_integration.py; then
    echo -e "\n  ${GREEN}✔  Integration tests PASSED${RESET}"
else
    echo -e "\n  ${RED}✘  Integration tests FAILED${RESET}"; INT_FAILED=1
fi
sep
if [[ $UNIT_FAILED -eq 0 && $INT_FAILED -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  ALL TESTS PASSED${RESET}"; sep; exit 0
else
    echo -e "${BOLD}${RED}  SOME TESTS FAILED${RESET}"; sep; exit 1
fi
