#!/usr/bin/env bash
# FluxDM — tests/run_extension_tests.sh
# The browser extension: its background script against a stand-in for the chrome.* APIs (node --test), and its
# native-messaging host as a real process over pipes (python3 unittest). Needs Node.js 18+ and Python 3.10+; installs
# nothing and builds nothing.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"; BOLD="\033[1m"; RESET="\033[0m"
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }

command -v node >/dev/null 2>&1    || { echo -e "  ${RED}✘${RESET}  node not found (Node.js 18 or newer)"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo -e "  ${RED}✘${RESET}  python3 not found (3.10 or newer)"; exit 1; }

status=0

info "Background script (node --test)"
node --test "$ROOT/tests/test_extension.js" || status=1

info "Native-messaging host (python3 unittest)"
python3 -I "$ROOT/tests/test_native_host.py" || status=1

echo
if [[ $status -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}EXTENSION — ALL TESTS PASSED ✔${RESET}"
else
    echo -e "  ${RED}${BOLD}EXTENSION — SOME TESTS FAILED${RESET}"
fi
exit $status
