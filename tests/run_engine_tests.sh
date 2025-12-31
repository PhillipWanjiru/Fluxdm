#!/usr/bin/env bash
# FluxDM — tests/run_engine_tests.sh
# Builds the Phase 2 C++ engine (SegmentManager, AdaptiveTuner, BufferManager,
# MetadataParser, TorrentEngine, PeerDiscovery) and the C core together,
# then runs test_engine.
#
# Does not require CMake — compiles directly with g++/gcc so it works
# on any machine with build-essential + libssl-dev installed.

set -euo pipefail

GREEN="\033[92m"; RED="\033[91m"; CYAN="\033[96m"
BOLD="\033[1m"; RESET="\033[0m"
ok()   { echo -e "  ${GREEN}✔${RESET}  $*"; }
info() { echo -e "\n${BOLD}${CYAN}── $*${RESET}"; }
err()  { echo -e "  ${RED}✘${RESET}  $*"; exit 1; }
sep()  { echo -e "${CYAN}${BOLD}────────────────────────────────────────────────${RESET}"; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/tests/.engine_build"

sep
echo -e "${BOLD}  FluxDM — Phase 2 Engine Build + Test${RESET}"
sep

# ── Dependency check ──────────────────────────────────────────────────────────
info "Checking dependencies"
command -v g++ >/dev/null 2>&1 || err "g++ not found. Run: sudo apt install build-essential"
pkg-config --exists openssl 2>/dev/null || err "OpenSSL not found. Run: sudo apt install libssl-dev"
ok "g++, OpenSSL"

mkdir -p "$BUILD_DIR/core"

CXXFLAGS="-std=c++17 -Wall -Wextra -g -O0 -D_GNU_SOURCE"
CFLAGS="-std=c11 -Wall -Wextra -D_GNU_SOURCE -g -O0"
INCLUDES="-I$ROOT/engine/include -I$ROOT/core/include -I$ROOT/ipc"

# ── Compile C core ────────────────────────────────────────────────────────────
info "Compiling C core"
C_SRCS="errors bandwidth checksum resume hostgate connection segment downloader"
for f in $C_SRCS; do
    gcc $CFLAGS $INCLUDES -c "$ROOT/core/src/${f}.c" -o "$BUILD_DIR/core/${f}.o"
    echo "  CC  ${f}.c"
done
gcc $CFLAGS $INCLUDES -c "$ROOT/core/src/protocol/http.c"       -o "$BUILD_DIR/core/http.o"
gcc $CFLAGS $INCLUDES -c "$ROOT/core/src/protocol/https.c"      -o "$BUILD_DIR/core/https.o"
gcc $CFLAGS $INCLUDES -c "$ROOT/core/src/ipc/frame_io.c"        -o "$BUILD_DIR/core/frame_io.o"
gcc $CFLAGS $INCLUDES -c "$ROOT/core/src/ipc/socket_server.c"   -o "$BUILD_DIR/core/socket_server.o"
echo "  CC  protocol/http.c, protocol/https.c, ipc/frame_io.c, ipc/socket_server.c"
ok "C core compiled"

# ── Compile C++ engine ────────────────────────────────────────────────────────
info "Compiling C++ engine (Phase 2)"
ENGINE_SRCS="SegmentManager AdaptiveTuner BufferManager MetadataParser TorrentEngine PeerDiscovery"
for f in $ENGINE_SRCS; do
    g++ $CXXFLAGS $INCLUDES -c "$ROOT/engine/src/${f}.cpp" -o "$BUILD_DIR/${f}.o"
    echo "  CXX ${f}.cpp"
done
ok "Engine compiled"

# ── Compile test file ─────────────────────────────────────────────────────────
info "Compiling test_engine.cpp"
g++ $CXXFLAGS $INCLUDES -c "$ROOT/tests/test_engine.cpp" -o "$BUILD_DIR/test_engine.o"
ok "test_engine.cpp compiled"

# ── Link ──────────────────────────────────────────────────────────────────────
info "Linking test_engine"
g++ -o "$BUILD_DIR/test_engine" \
    "$BUILD_DIR/test_engine.o" \
    $(for f in $ENGINE_SRCS; do echo "$BUILD_DIR/${f}.o"; done) \
    "$BUILD_DIR"/core/*.o \
    -lssl -lcrypto -lpthread
ok "Linked → $BUILD_DIR/test_engine"
chmod +x "$BUILD_DIR/test_engine"  # defensive: some filesystems (network mounts,
                                    # certain WSL configs) don't preserve the exec
                                    # bit set by the linker

# ── Run ───────────────────────────────────────────────────────────────────────
sep
echo -e "${BOLD}  Running Phase 2 Engine Tests${RESET}"
sep
echo ""
"$BUILD_DIR/test_engine"
RC=$?

sep
if [[ $RC -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}PHASE 2 ENGINE — ALL TESTS PASSED ✔${RESET}"
else
    echo -e "  ${RED}Some Phase 2 tests failed — see output above${RESET}"
fi
sep

exit $RC
