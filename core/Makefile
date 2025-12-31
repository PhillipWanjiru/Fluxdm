# FluxDM — core/Makefile
# Builds the C core daemon: fluxdm-core
# Requires: gcc, libssl-dev (OpenSSL), libpthread (usually built-in)
#
# Targets:
#   make          — build release binary
#   make debug    — build with ASan + UBSan + debug symbols
#   make tsan     — build with ThreadSanitizer (run `make clean` first)
#   make clean    — remove build artefacts
#   make install  — copy binary to /usr/local/bin

# ── Toolchain ─────────────────────────────────────────────────────────────────
CC      := gcc
AR      := ar

# ── Directories ───────────────────────────────────────────────────────────────
SRC_DIR   := src
INC_DIR   := include
IPC_SRC   := src/ipc
PROTO_SRC := src/protocol
IPC_INC   := ../ipc
BUILD_DIR := build
BIN       := fluxdm-core

# ── Sources ───────────────────────────────────────────────────────────────────
SRCS := \
	$(SRC_DIR)/main.c          \
	$(SRC_DIR)/downloader.c    \
	$(SRC_DIR)/segment.c       \
	$(SRC_DIR)/connection.c    \
	$(SRC_DIR)/resume.c        \
	$(SRC_DIR)/bandwidth.c     \
	$(SRC_DIR)/checksum.c      \
	$(SRC_DIR)/errors.c        \
	$(PROTO_SRC)/http.c        \
	$(PROTO_SRC)/https.c       \
	$(IPC_SRC)/frame_io.c      \
	$(IPC_SRC)/socket_server.c

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

# ── Flags ─────────────────────────────────────────────────────────────────────
CFLAGS_COMMON := \
	-std=c11                    \
	-Wall                       \
	-Wextra                     \
	-Wpedantic                  \
	-Wshadow                    \
	-Wformat=2                  \
	-Wstrict-prototypes         \
	-Wmissing-prototypes        \
	-fstack-protector-strong    \
	-D_GNU_SOURCE               \
	-D_FORTIFY_SOURCE=2         \
	-I$(INC_DIR)                \
	-I$(IPC_INC)

CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -DNDEBUG -flto
CFLAGS_DEBUG   := $(CFLAGS_COMMON) -Og -g3 -DDEBUG \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer

LDFLAGS_COMMON := -lpthread -lssl -lcrypto
LDFLAGS_RELEASE := $(LDFLAGS_COMMON) -flto
LDFLAGS_DEBUG   := $(LDFLAGS_COMMON) -fsanitize=address,undefined

# Default: release
CFLAGS  := $(CFLAGS_RELEASE)
LDFLAGS := $(LDFLAGS_RELEASE)

# ── Rules ─────────────────────────────────────────────────────────────────────
.PHONY: all debug tsan clean install check-deps

all: check-deps $(BIN)
	@echo ""
	@echo "  ✔  Built $(BIN) (release)"
	@echo "     Run: ./$(BIN) --help"
	@echo ""

debug: CFLAGS  := $(CFLAGS_DEBUG)
debug: LDFLAGS := $(LDFLAGS_DEBUG)
debug: $(BIN)
	@echo "  ✔  Built $(BIN) (debug + ASan + UBSan)"

# Data-race checking: `make clean && make tsan`
tsan: CFLAGS  := $(CFLAGS_COMMON) -O1 -g3 -DDEBUG -fsanitize=thread -fno-omit-frame-pointer
tsan: LDFLAGS := $(LDFLAGS_COMMON) -fsanitize=thread
tsan: $(BIN)
	@echo "  ✔  Built $(BIN) (ThreadSanitizer)"

$(BIN): $(OBJS)
	@echo "  LD  $@"
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Dependency generation (header changes trigger recompile) ──────────────────
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# ── Utility targets ───────────────────────────────────────────────────────────
clean:
	@rm -rf $(BUILD_DIR) $(BIN)
	@echo "  ✔  Cleaned"

install: $(BIN)
	@install -m 755 $(BIN) /usr/local/bin/$(BIN)
	@echo "  ✔  Installed to /usr/local/bin/$(BIN)"

check-deps:
	@command -v $(CC) >/dev/null 2>&1 || \
		{ echo "  ✘  gcc not found. Run: sudo apt install build-essential"; exit 1; }
	@pkg-config --exists openssl 2>/dev/null || \
		{ echo "  ✘  OpenSSL not found. Run: sudo apt install libssl-dev"; exit 1; }
	@echo "  ✔  Dependencies OK"
