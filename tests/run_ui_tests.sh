#!/usr/bin/env bash
# FluxDM — tests/run_ui_tests.sh
# Runs the UI's checks the way CI's `ui` job does:
#   1. the unit and component tests (Jest, through react-scripts)
#   2. the production build, with warnings treated as errors (CI=true)
#
#   bash tests/run_ui_tests.sh              everything
#   bash tests/run_ui_tests.sh --no-build   skip the production build
#
# Needs Node.js 20 or newer. When ui/node_modules does not exist yet the dependencies are installed with
# `npm ci`, the exact versions in ui/package-lock.json. Set ELECTRON_SKIP_BINARY_DOWNLOAD=1 first if you only
# want the tests and not Electron's ~100 MB binary.

set -euo pipefail

GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"
BOLD="\033[1m"; RESET="\033[0m"
ok()   { echo -e "  ${GREEN}✔${RESET}  $*"; }
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }
err()  { echo -e "  ${RED}✘${RESET}  $*"; exit 1; }
sep()  { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI="$ROOT/ui"

BUILD=1
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=0 ;;
        -h|--help)  sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          err "unknown option: $arg (try --help)" ;;
    esac
done

sep
echo -e "${BOLD}  FluxDM — UI tests${RESET}"
sep

# ── Dependency check ──────────────────────────────────────────────────────────
info "Checking dependencies"
command -v node >/dev/null 2>&1 || err "node not found. Install Node.js 20 or newer (https://nodejs.org or your package manager)."
command -v npm  >/dev/null 2>&1 || err "npm not found. It ships with Node.js."
node_major="$(node -p 'process.versions.node.split(".")[0]')"
[[ "$node_major" -ge 20 ]] || err "Node.js $node_major found, 20 or newer is needed ($(node -v))."
ok "node $(node -v), npm $(npm -v)"

if [[ ! -d "$UI/node_modules" ]]; then
    info "Installing the UI dependencies (npm ci)"
    (cd "$UI" && npm ci --no-audit --no-fund) || err "npm ci failed. Is ui/package-lock.json in step with ui/package.json?"
    ok "dependencies installed"
fi

# ── Unit and component tests ──────────────────────────────────────────────────
info "Unit and component tests (Jest)"
(cd "$UI" && CI=true npm test -- --watchAll=false) || err "Jest tests failed"
ok "Jest tests passed"

# ── Production build ──────────────────────────────────────────────────────────
if [[ "$BUILD" -eq 1 ]]; then
    info "Production build (warnings are errors, as in CI)"
    (cd "$UI" && CI=true npm run build) || err "production build failed"
    ok "build/ produced"
else
    echo "  –  production build skipped (--no-build)"
fi

sep
echo -e "${GREEN}${BOLD}  UI checks passed${RESET}"
sep
