#!/usr/bin/env bash
# FluxDM — dev_start.sh
# One command to always get a running daemon, regardless of whether the
# native fluxdm-core binary exists yet on this machine/checkout.
#
# `make` is idempotent — if core/ hasn't changed since the last build, it
# does effectively nothing and returns instantly. So this is always safe
# and always cheap to run, even if fluxdm-core is already built.
#
# Usage:
#   bash dev_start.sh          — rebuild core (if needed) + run the daemon
#   bash dev_start.sh --build  — just rebuild core, don't launch anything

set -euo pipefail
GREEN="\033[92m"; CYAN="\033[96m"; BOLD="\033[1m"; RESET="\033[0m"
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }
ok()   { echo -e "  ${GREEN}✔${RESET}  $*"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info "Ensuring fluxdm-core is built"
cd "$ROOT/core"
make
ok "fluxdm-core ready at $ROOT/core/fluxdm-core"

if [[ "${1:-}" == "--build" ]]; then
    exit 0
fi

info "Building + launching daemon"
cd "$ROOT/daemon"

# On a brand-new machine, ~/.m2/repository is empty and Maven has to
# download every dependency from Maven Central before it can compile
# anything. The -q flag below keeps normal rebuilds quiet, but that
# same silence makes a first-time multi-minute download look exactly
# like the script has hung. Warn up front so it's clear what's
# happening instead of leaving you staring at a blank terminal.
M2_REPO="$HOME/.m2/repository/org/springframework/boot/spring-boot-starter-parent"
if [[ ! -d "$M2_REPO" ]]; then
    echo ""
    echo -e "  ${CYAN}First build on this machine — Maven needs to download all"
    echo -e "  dependencies from Maven Central. This can take a few minutes"
    echo -e "  depending on connection speed. This is normal, not a hang.${RESET}"
    echo ""
fi

mvn -q -DskipTests package
ok "daemon built"

echo ""
exec java -jar target/fluxdm-daemon.jar
