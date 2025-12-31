# FluxDM — tests/Makefile
#
#   make unit          build test_unit     (errors, bandwidth, checksum, resume, http, host gate ...)
#   make unit-io       build test_unit_io  (URL handling, HTTP head parser, IPC framing)
#   make unit-net      build test_unit_net (the same layers from another angle, plus the limiter's burst depth)
#   make run-unit      build and run all three unit suites
#   make integration   smoke tests against a private core instance (needs ../core/fluxdm-core)
#   make core-tests    the full behavioural suite (several minutes; add QUICK=1 for a shorter run)
#   make clean
#
# Everything is built with AddressSanitizer + UndefinedBehaviorSanitizer.

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g3 -O0 -D_GNU_SOURCE \
           -I../core/include -I../core/src -I../ipc \
           -fsanitize=address,undefined \
           -fno-omit-frame-pointer
LDFLAGS := -lpthread -lssl -lcrypto \
           -fsanitize=address,undefined

SRCS := test_unit.c \
        ../core/src/errors.c \
        ../core/src/bandwidth.c \
        ../core/src/checksum.c \
        ../core/src/resume.c \
        ../core/src/hostgate.c \
        ../core/src/protocol/http.c \
        ../core/src/protocol/https.c \
        ../core/src/connection.c

IO_SRCS := test_unit_io.c \
        ../core/src/errors.c \
        ../core/src/protocol/http.c \
        ../core/src/protocol/https.c \
        ../core/src/connection.c \
        ../core/src/ipc/frame_io.c

NET_SRCS := test_unit_net.c \
        ../core/src/errors.c \
        ../core/src/bandwidth.c \
        ../core/src/protocol/http.c \
        ../core/src/protocol/https.c \
        ../core/src/connection.c \
        ../core/src/ipc/frame_io.c

.PHONY: all unit unit-io unit-net run-unit integration core-tests clean

all: unit unit-io unit-net

unit: $(SRCS)
	@echo "  CC  test_unit"
	@$(CC) $(CFLAGS) -o test_unit $(SRCS) $(LDFLAGS)
	@echo "  ✔  Built test_unit (ASan + UBSan enabled)"

unit-io: $(IO_SRCS) ../core/src/ipc/frame_io.h
	@echo "  CC  test_unit_io"
	@$(CC) $(CFLAGS) -o test_unit_io $(IO_SRCS) $(LDFLAGS)
	@echo "  ✔  Built test_unit_io (ASan + UBSan enabled)"

unit-net: $(NET_SRCS) ../core/src/ipc/frame_io.h
	@echo "  CC  test_unit_net"
	@$(CC) $(CFLAGS) -o test_unit_net $(NET_SRCS) $(LDFLAGS)
	@echo "  ✔  Built test_unit_net (ASan + UBSan enabled)"

run-unit: unit unit-io unit-net
	@./test_unit
	@./test_unit_io
	@./test_unit_net

integration:
	@echo "  PY  test_integration.py"
	@python3 test_integration.py

core-tests:
	@python3 test_core.py $(if $(QUICK),--quick,)

clean:
	@rm -f test_unit test_unit_io test_unit_net
	@echo "  ✔  Cleaned"
