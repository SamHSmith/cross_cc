# cross_cc: bootstrap with the system C compiler only.
#   make setup   fetch + verify upstream sources AND the pinned LLVM 22.1.8 SDK, build the
#                embedded backend (LLVM=system skips the download and uses the installed clang++;
#                LLVM_PREFIX=/path reuses an existing LLVM 22.1.8 static install)
#   make         build bin/cross_cc plus its argv0 aliases (runs native first if needed)
#   make test    windows_path tests, every tests/*_test.c unit program, then the smoke and
#                tests/*_integration.c programs (needs `make REFERENCE=1 setup` once)
#   make native  (re)build only the embedded Clang/LLD/LLVM backend archive
#   make package / make release ARGS='...'   distribution bundle / cross release
CC ?= cc
CFLAGS ?= -O1
JOBS ?=
LLVM ?= pinned
REFERENCE ?=
TOOLDIR = build/tools
TOOLS = $(TOOLDIR)/build $(TOOLDIR)/release $(TOOLDIR)/linux-release-deps $(TOOLDIR)/macos-x64-llvm
BUILD = $(TOOLDIR)/build
JOBS_ARG = $(if $(JOBS),--jobs=$(JOBS))
NATIVE_DIR ?= .cache/native
NATIVE_FLAGS = $(NATIVE_DIR)/linker-flags.txt
NATIVE_ARG = --native-dir=$(NATIVE_DIR)

.PHONY: all setup native build test package release tools check-tools clean help

all: build

tools: $(TOOLS)

$(TOOLDIR)/%: tools/%.c tools/common.h
	@mkdir -p $(TOOLDIR)
	$(CC) -std=c11 $(CFLAGS) -o $@ $<

$(TOOLDIR)/build: tools/windows_path/windows_path.c
$(TOOLDIR)/release: tools/windows_path/windows_path.c

setup: $(BUILD)
	$(BUILD) setup $(JOBS_ARG) --llvm=$(LLVM) $(NATIVE_ARG) $(if $(REFERENCE),--reference)

native: $(BUILD)
	$(BUILD) native $(JOBS_ARG) --llvm=$(LLVM) $(NATIVE_ARG)

$(NATIVE_FLAGS): $(BUILD)
	$(BUILD) native $(JOBS_ARG) --llvm=$(LLVM) $(NATIVE_ARG)

build: $(BUILD) $(NATIVE_FLAGS)
	$(BUILD) build $(JOBS_ARG) $(NATIVE_ARG)

test: $(BUILD)
	$(BUILD) test $(JOBS_ARG)

package: $(BUILD)
	$(BUILD) package $(ARGS)

release: $(TOOLDIR)/release $(BUILD)
	$(TOOLDIR)/release $(ARGS)

# Unit tests for the Windows path helpers only (no network, no LLVM needed).
check-tools: $(TOOLDIR)/windows_path_test
	$(TOOLDIR)/windows_path_test

$(TOOLDIR)/windows_path_test: tools/windows_path/windows_path_test.c tools/windows_path/windows_path.c tools/common.h
	@mkdir -p $(TOOLDIR)
	$(CC) -std=c11 $(CFLAGS) -o $@ $<

help: $(BUILD)
	$(BUILD) help

clean:
	rm -rf $(TOOLDIR)
