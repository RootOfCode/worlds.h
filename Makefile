# ─────────────────────────────────────────────────────────────────────────────
# Makefile — worlds.h example
#
# Targets
#   make / make all   Build the example (C11, optimised)
#   make run          Build and run
#   make c99          Build with -std=c99 (no threads.h, no _Thread_local)
#   make debug        Build with debug symbols and sanitisers
#   make check        Build and run both the C11 and C99 variants
#   make clean        Remove all build artefacts
#   make help         Print this summary
# ─────────────────────────────────────────────────────────────────────────────

# ── Toolchain ─────────────────────────────────────────────────────────────────
CC      ?= cc
CFLAGS  ?= -O2

# ── Common flags applied to every build ──────────────────────────────────────
WARN    := -Wall -Wextra -Wpedantic \
           -Wmissing-prototypes \
           -Wstrict-prototypes \
           -Wold-style-definition

# Standard C library — link pthreads on Linux for the C11 threads shim
# (on macOS and musl libc this is a no-op / already included).
LIBS    :=
UNAME   := $(shell uname -s 2>/dev/null || echo unknown)
ifeq ($(UNAME),Linux)
    LIBS += -lpthread
endif

# ── Source / output ───────────────────────────────────────────────────────────
SRC     := example.c
HDR     := worlds.h
BIN     := example
BIN_C99 := example_c99
BIN_DBG := example_debug

BUILDDIR := build

# ─────────────────────────────────────────────────────────────────────────────
# Primary targets
# ─────────────────────────────────────────────────────────────────────────────

.PHONY: all run c99 debug check clean help

all: $(BUILDDIR)/$(BIN)

$(BUILDDIR)/$(BIN): $(SRC) $(HDR) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ $(SRC) $(LIBS)

run: $(BUILDDIR)/$(BIN)
	@echo "── running $(BIN) ──────────────────────────────────────────────"
	@$(BUILDDIR)/$(BIN)

# ── C99 variant ───────────────────────────────────────────────────────────────
# C99 disables _Thread_local and <threads.h>; the library silently falls back
# to a plain global current-world pointer and no-op mutexes.

c99: $(BUILDDIR)/$(BIN_C99)

$(BUILDDIR)/$(BIN_C99): $(SRC) $(HDR) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(WARN) -std=c99 -o $@ $(SRC) $(LIBS)

# ── Debug variant ─────────────────────────────────────────────────────────────
# Adds -g, -fsanitize=address,undefined.  ASan/UBSan require GCC ≥ 4.8 or
# Clang ≥ 3.1.  On macOS you may need to install the Xcode command-line tools.

SANFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

debug: $(BUILDDIR)/$(BIN_DBG)

$(BUILDDIR)/$(BIN_DBG): $(SRC) $(HDR) | $(BUILDDIR)
	$(CC) $(SANFLAGS) $(WARN) -std=c11 -o $@ $(SRC) $(LIBS)

# ── check: build and run both standard variants ───────────────────────────────

check: $(BUILDDIR)/$(BIN) $(BUILDDIR)/$(BIN_C99)
	@echo ""
	@echo "══ C11 ══════════════════════════════════════════════════════════"
	@$(BUILDDIR)/$(BIN)
	@echo ""
	@echo "══ C99 ══════════════════════════════════════════════════════════"
	@$(BUILDDIR)/$(BIN_C99)
	@echo ""
	@echo "All checks passed."

# ── Helpers ───────────────────────────────────────────────────────────────────

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

help:
	@echo "worlds.h — available make targets"
	@echo ""
	@echo "  make / make all   Build example with -std=c11  →  build/$(BIN)"
	@echo "  make run          Build and run"
	@echo "  make c99          Build with -std=c99           →  build/$(BIN_C99)"
	@echo "  make debug        Build with ASan + UBSan       →  build/$(BIN_DBG)"
	@echo "  make check        Build and run both c11 and c99 variants"
	@echo "  make clean        Remove build/ directory"
	@echo "  make help         Show this message"
	@echo ""
	@echo "  CC=$(CC)  CFLAGS=$(CFLAGS)"
