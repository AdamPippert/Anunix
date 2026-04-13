# Anunix top-level Makefile
#
# Build targets:
#   make kernel            Build kernel for ARCH (default: host arch)
#   make kernel ARCH=arm64
#   make kernel ARCH=x86_64
#   make qemu              Boot kernel in QEMU (headless, serial console)
#   make clean             Remove all build artifacts
#   make test              Run kernel unit tests (host-native)
#   make toolchain         Fetch LLVM tools (ld.lld, llvm-objcopy)
#   make toolchain-check   Verify build dependencies
#
# VM testing:
#   UTM on M1 Mac Studio — ARM64 native, x86_64 emulated
#   QEMU/libvirt on Jekyll (Linux) — both architectures
#
# Prerequisites:
#   make toolchain         (one-time: fetches ld.lld + llvm-objcopy)
#   QEMU installed separately for 'make qemu'

# --- Architecture detection ---
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  HOST_ARCH := arm64
else ifeq ($(UNAME_M),aarch64)
  HOST_ARCH := arm64
else
  HOST_ARCH := x86_64
endif

ARCH ?= $(HOST_ARCH)

# --- Toolchain ---
# Apple's Xcode/CLT clang supports both targets but lacks ld.lld and
# llvm-objcopy. Run 'make toolchain' to fetch them into tools/llvm/bin/.
LOCAL_LLVM := tools/llvm/bin

# Use system clang (Xcode/CLT), local LLVM for linker and objcopy
CC      := clang
AS      := clang

# Prefer local LLVM tools, fall back to PATH
ifneq ($(wildcard $(LOCAL_LLVM)/ld.lld),)
  LD      := $(LOCAL_LLVM)/ld.lld
  OBJCOPY := $(LOCAL_LLVM)/llvm-objcopy
else
  LD      := ld.lld
  OBJCOPY := llvm-objcopy
endif

ifeq ($(ARCH),arm64)
  TARGET  := aarch64-none-elf
  QEMU    := qemu-system-aarch64
  QFLAGS  := -M virt -cpu cortex-a72 -m 512M -nographic -kernel
else ifeq ($(ARCH),x86_64)
  TARGET  := x86_64-none-elf
  QEMU    := qemu-system-x86_64
  QFLAGS  := -m 512M -nographic -no-reboot -kernel
else
  $(error Unknown ARCH=$(ARCH). Use arm64 or x86_64)
endif

CFLAGS  := -target $(TARGET) \
           -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -Wall -Wextra -Werror -std=c11 \
           -I kernel/include \
           -O2 -g

ASFLAGS := -target $(TARGET) \
           -ffreestanding -nostdlib

LDFLAGS := -nostdlib --gc-sections

# --- Source files ---
ARCH_DIR  := kernel/arch/$(ARCH)
CORE_DIR  := kernel/core
LIB_DIR   := kernel/lib
BUILD_DIR := build/$(ARCH)
LINK_LD   := $(ARCH_DIR)/link.ld

# Collect sources (use shell find for recursive, wildcard doesn't recurse)
ARCH_C    := $(wildcard $(ARCH_DIR)/*.c)
ARCH_S    := $(wildcard $(ARCH_DIR)/*.S)
CORE_C    := $(shell find $(CORE_DIR) -name '*.c' 2>/dev/null)
LIB_C     := $(wildcard $(LIB_DIR)/*.c)

# Object files
ARCH_C_OBJ := $(patsubst $(ARCH_DIR)/%.c,$(BUILD_DIR)/arch/%.o,$(ARCH_C))
ARCH_S_OBJ := $(patsubst $(ARCH_DIR)/%.S,$(BUILD_DIR)/arch/%.o,$(ARCH_S))
CORE_OBJ   := $(patsubst $(CORE_DIR)/%.c,$(BUILD_DIR)/core/%.o,$(CORE_C))
LIB_OBJ    := $(patsubst $(LIB_DIR)/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_C))

ALL_OBJ    := $(ARCH_S_OBJ) $(ARCH_C_OBJ) $(CORE_OBJ) $(LIB_OBJ)

KERNEL_ELF := $(BUILD_DIR)/anunix.elf
KERNEL_BIN := $(BUILD_DIR)/anunix.bin

# --- Targets ---
.PHONY: kernel qemu clean test toolchain toolchain-check proto-install proto-test

kernel: $(KERNEL_BIN)
	@echo "  BUILT   $(KERNEL_BIN) [$(ARCH)]"

$(KERNEL_BIN): $(KERNEL_ELF)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary $< $@

$(KERNEL_ELF): $(ALL_OBJ) $(LINK_LD)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINK_LD) -o $@ $(ALL_OBJ)

# Compile C files from arch/
$(BUILD_DIR)/arch/%.o: $(ARCH_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble .S files from arch/
$(BUILD_DIR)/arch/%.o: $(ARCH_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# Compile C files from core/ (recursive)
$(BUILD_DIR)/core/%.o: $(CORE_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C files from lib/
$(BUILD_DIR)/lib/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

qemu: $(KERNEL_BIN)
	$(QEMU) $(QFLAGS) $(KERNEL_BIN)

# Fetch LLVM tools (ld.lld, llvm-objcopy) into tools/llvm/bin/
toolchain:
	@./tools/fetch-llvm.sh

toolchain-check:
	@echo "Checking build dependencies..."
	@$(CC) --version > /dev/null 2>&1 || (echo "MISSING: clang — install Xcode Command Line Tools" && exit 1)
	@echo "  clang:        $(shell $(CC) --version 2>&1 | head -1)"
	@$(LD) --version > /dev/null 2>&1 || (echo "MISSING: ld.lld — run 'make toolchain'" && exit 1)
	@echo "  ld.lld:       $(shell $(LD) --version 2>&1 | head -1)"
	@$(OBJCOPY) --version > /dev/null 2>&1 || (echo "MISSING: llvm-objcopy — run 'make toolchain'" && exit 1)
	@echo "  llvm-objcopy: $(shell $(OBJCOPY) --version 2>&1 | head -1)"
	@which $(QEMU) > /dev/null 2>&1 && echo "  qemu:         $(shell $(QEMU) --version 2>&1 | head -1)" || echo "  qemu:         not found (needed for 'make qemu')"
	@echo "Toolchain OK."

clean:
	rm -rf build/
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	find . -type d -name .mypy_cache -exec rm -rf {} + 2>/dev/null || true
	find . -type d -name .pytest_cache -exec rm -rf {} + 2>/dev/null || true
	rm -rf dist/ *.egg-info

test:
	@echo "TODO: kernel unit test harness"

# --- Python prototype (legacy) ---
proto-install:
	pip install -e ".[all]"

proto-test:
	python -m pytest tests/ -v
