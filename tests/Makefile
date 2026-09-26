# FluxDM — tests/Makefile

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
        ../core/src/protocol/http.c \
        ../core/src/protocol/https.c \
        ../core/src/connection.c

.PHONY: unit integration clean all

all: unit

unit: $(SRCS)
	@echo "  CC  test_unit"
	@$(CC) $(CFLAGS) -o test_unit $(SRCS) $(LDFLAGS)
	@echo "  ✔  Built test_unit (ASan + UBSan enabled)"

integration:
	@echo "  PY  test_integration.py"
	@python3 test_integration.py

clean:
	@rm -f test_unit
	@echo "  ✔  Cleaned"
