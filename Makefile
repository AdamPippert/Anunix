# Anunix top-level Makefile
#
# Build targets:
#   make kernel            Build kernel for ARCH (default: host arch)
#   make kernel ARCH=arm64
#   make kernel ARCH=x86_64
#   make qemu              Boot kernel in QEMU (headless, serial console)
#   make qemu-deps         Build QEMU and dependencies from source
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
ANX_VERSION := 2026.4.17

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

LOCAL_QEMU := tools/qemu/bin

ifeq ($(ARCH),arm64)
  TARGET  := aarch64-none-elf
  ifneq ($(wildcard $(LOCAL_QEMU)/qemu-system-aarch64),)
    QEMU  := $(LOCAL_QEMU)/qemu-system-aarch64
  else
    QEMU  := qemu-system-aarch64
  endif
  QFLAGS  := -M virt -cpu cortex-a72 -m 512M -nographic -serial mon:stdio -kernel
else ifeq ($(ARCH),x86_64)
  TARGET  := x86_64-none-elf
  ifneq ($(wildcard $(LOCAL_QEMU)/qemu-system-x86_64),)
    QEMU  := $(LOCAL_QEMU)/qemu-system-x86_64
  else
    QEMU  := qemu-system-x86_64
  endif
  QFLAGS  := -m 512M -nographic -no-reboot -serial mon:stdio -kernel
else ifeq ($(ARCH),heteris)
  # Heteris: RV64IM cross-compilation using xPack riscv-none-elf-gcc
  # Install toolchain: download xPack to tools/ or set RISCV_PREFIX
  RISCV_TOOLS := $(wildcard tools/xpack-riscv-none-elf-gcc-*/bin)
  HETERIS_ISA := ../heteris-isa
  RISCV_PREFIX ?= $(if $(RISCV_TOOLS),$(RISCV_TOOLS)/riscv-none-elf-,riscv-none-elf-)
  CC      := $(RISCV_PREFIX)gcc
  AS      := $(RISCV_PREFIX)gcc
  LD      := $(RISCV_PREFIX)ld
  OBJCOPY := $(RISCV_PREFIX)objcopy
  TARGET  := riscv64-none-elf
  QEMU    := @echo "Use $(HETERIS_ISA)/sim simulator for Heteris" && false
  QFLAGS  :=
else
  $(error Unknown ARCH=$(ARCH). Use arm64, x86_64, or heteris)
endif

ifeq ($(ARCH),heteris)
CFLAGS  := -march=rv64ima_zicsr -mabi=lp64 \
           -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -Wall -Wextra -Werror -std=c11 \
           -I kernel/include \
           -O2 -g

ASFLAGS := -march=rv64ima_zicsr -mabi=lp64 \
           -ffreestanding -nostdlib

LDFLAGS := -nostdlib --gc-sections -m elf64lriscv
else
CFLAGS  := -target $(TARGET) \
           -ffreestanding -fno-builtin -nostdlib -nostdinc \
           -Wall -Wextra -Werror -std=c11 \
           -mgeneral-regs-only \
           -I kernel/include \
           -O2 -g

ASFLAGS := -target $(TARGET) \
           -ffreestanding -nostdlib

LDFLAGS := -nostdlib --gc-sections
endif

# --- Source files ---
ARCH_DIR    := kernel/arch/$(ARCH)
CORE_DIR    := kernel/core
LIB_DIR     := kernel/lib
DRIVER_DIR  := kernel/drivers
BUILD_DIR   := build/$(ARCH)
LINK_LD     := $(ARCH_DIR)/link.ld

# Collect sources (use shell find for recursive, wildcard doesn't recurse)
ARCH_C    := $(wildcard $(ARCH_DIR)/*.c)
ARCH_S    := $(filter-out %/qemu_boot.S, $(wildcard $(ARCH_DIR)/*.S))
CORE_C    := $(shell find $(CORE_DIR) -name '*.c' 2>/dev/null)
LIB_C     := $(shell find $(LIB_DIR) -name '*.c' 2>/dev/null)
DRIVER_C  := $(shell find $(DRIVER_DIR) -name '*.c' 2>/dev/null)
DRIVER_S  := $(shell find $(DRIVER_DIR) -name '*.S' 2>/dev/null)

# Object files
ARCH_C_OBJ  := $(patsubst $(ARCH_DIR)/%.c,$(BUILD_DIR)/arch/%.o,$(ARCH_C))
ARCH_S_OBJ  := $(patsubst $(ARCH_DIR)/%.S,$(BUILD_DIR)/arch/%.o,$(ARCH_S))
CORE_OBJ    := $(patsubst $(CORE_DIR)/%.c,$(BUILD_DIR)/core/%.o,$(CORE_C))
LIB_OBJ     := $(patsubst $(LIB_DIR)/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_C))
DRIVER_OBJ  := $(patsubst $(DRIVER_DIR)/%.c,$(BUILD_DIR)/drivers/%.o,$(DRIVER_C))
DRIVER_S_OBJ := $(patsubst $(DRIVER_DIR)/%.S,$(BUILD_DIR)/drivers/%.o,$(DRIVER_S))

ALL_OBJ    := $(ARCH_S_OBJ) $(ARCH_C_OBJ) $(CORE_OBJ) $(DRIVER_OBJ) $(DRIVER_S_OBJ) $(LIB_OBJ)

KERNEL_ELF := $(BUILD_DIR)/anunix.elf
KERNEL_BIN := $(BUILD_DIR)/anunix.bin

# --- Targets ---
.PHONY: kernel qemu qemu-fb qemu-deps clean test toolchain toolchain-check iso iso-deps dist proto-install proto-test

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

# Compile C files from lib/ (recursive, handles subdirs like crypto/)
$(BUILD_DIR)/lib/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C files from drivers/ (recursive)
$(BUILD_DIR)/drivers/%.o: $(DRIVER_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble .S files from drivers/ (for embedded assets)
$(BUILD_DIR)/drivers/%.o: $(DRIVER_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# x86_64 QEMU needs a 32-bit multiboot wrapper (QEMU rejects ELF64 multiboot).
# ARM64 can use the ELF directly.
QEMU_KERNEL := $(KERNEL_ELF)

ifeq ($(ARCH),x86_64)
  QEMU_BOOT_SRC := $(ARCH_DIR)/qemu_boot.S
  QEMU_BOOT_LD  := $(ARCH_DIR)/qemu_link.ld
  QEMU_BOOT_OBJ := $(BUILD_DIR)/qemu_boot.o
  QEMU_KERNEL   := $(BUILD_DIR)/anunix-qemu.elf

$(QEMU_BOOT_OBJ): $(QEMU_BOOT_SRC) $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	$(CC) -target i686-none-elf -ffreestanding -nostdlib -c $< -o $@

$(QEMU_KERNEL): $(QEMU_BOOT_OBJ) $(QEMU_BOOT_LD)
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -T $(QEMU_BOOT_LD) -o $@ $(QEMU_BOOT_OBJ)
endif

qemu: $(QEMU_KERNEL)
	$(QEMU) $(QFLAGS) $(QEMU_KERNEL)

# QEMU with framebuffer display (serial on stdio + graphical window)
ifeq ($(ARCH),arm64)
  QFLAGS_FB := -M virt -cpu cortex-a72 -m 512M -serial mon:stdio -device ramfb -kernel
else ifeq ($(ARCH),x86_64)
  QFLAGS_FB := -m 512M -serial mon:stdio -no-reboot -vga std -kernel
else
  QFLAGS_FB := $(QFLAGS)
endif

qemu-fb: $(QEMU_KERNEL)
	$(QEMU) $(QFLAGS_FB) $(QEMU_KERNEL)

# Build QEMU and dependencies from source into tools/qemu/
qemu-deps:
	@./tools/build-qemu.sh

# --- ISO and distribution ---

# Download GRUB modules and build xorriso
iso-deps:
	@./tools/fetch-grub.sh

# EFI stub loader — standalone UEFI application that boots the kernel
EFI_STUB_DIR := kernel/boot/efi
EFI_STUB_EFI := build/x86_64/BOOTX64.EFI

# Build EFI stub as COFF objects → PE32+ via lld-link mode
$(EFI_STUB_EFI): $(KERNEL_BIN) $(EFI_STUB_DIR)/efi_stub.c $(EFI_STUB_DIR)/kernel_payload.S
	@mkdir -p $(dir $@)
	@echo "  EFI STUB"
	$(CC) -target x86_64-unknown-windows -ffreestanding -fno-builtin \
		-nostdlib -nostdinc -mno-red-zone -fno-stack-protector \
		-I $(EFI_STUB_DIR) -O2 -Wall -Wextra \
		-c $(EFI_STUB_DIR)/efi_stub.c \
		-o build/x86_64/efi_stub.obj
	$(CC) -target x86_64-unknown-windows -ffreestanding -nostdlib \
		-c $(EFI_STUB_DIR)/kernel_payload.S \
		-o build/x86_64/efi_payload.obj
	$(LD) -flavor link \
		/entry:efi_main /subsystem:efi_application \
		/out:$@ \
		build/x86_64/efi_stub.obj build/x86_64/efi_payload.obj

efi-stub: $(EFI_STUB_EFI)
	@echo "  BUILT   $(EFI_STUB_EFI)"

# Build bootable x86_64 hybrid ISO (BIOS + UEFI)
# Primary kernel: native 64-bit ELF (multiboot2, UEFI boot)
# Optional: 32-bit multiboot wrapper for legacy BIOS fallback
# Optional: EFI stub for direct UEFI boot without GRUB
iso:
	@$(MAKE) kernel ARCH=x86_64
	@$(MAKE) ARCH=x86_64 build/x86_64/anunix-qemu.elf 2>/dev/null || true
	@$(MAKE) ARCH=x86_64 efi-stub 2>/dev/null || true
	@./tools/build-iso.sh

# Distribution tarball with both architectures
dist: iso
	@echo "  Building distribution..."
	@mkdir -p build/dist/anunix-$(ANX_VERSION)/x86_64
	@mkdir -p build/dist/anunix-$(ANX_VERSION)/arm64
	@cp build/anunix-x86_64.iso build/dist/anunix-$(ANX_VERSION)/x86_64/
	@cp build/x86_64/anunix.elf build/dist/anunix-$(ANX_VERSION)/x86_64/
	@$(MAKE) kernel ARCH=arm64
	@cp build/arm64/anunix.elf build/dist/anunix-$(ANX_VERSION)/arm64/
	@cp build/arm64/anunix.bin build/dist/anunix-$(ANX_VERSION)/arm64/
	@cd build/dist && tar czf ../anunix-$(ANX_VERSION).tar.gz anunix-$(ANX_VERSION)/
	@echo "  DIST    build/anunix-$(ANX_VERSION).tar.gz"


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

# Host-native test build — uses system clang without freestanding/cross flags.
# Excludes kernel/core/main.c (test_main.c provides kernel_main).
TEST_CC     := clang
TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O0 -I kernel/include
TEST_CORE   := $(filter-out $(CORE_DIR)/main.c, \
		  $(filter-out $(CORE_DIR)/agent/%, $(CORE_C)))
# Exclude hardware-dependent drivers from host-native test builds.
# PCI, virtio, and net drivers use I/O ports and DMA — not testable on host.
# Exclude hardware-dependent drivers from host-native test builds.
# fb/*.c included (tests need it) except gui.c (needs kernel GUI subsystem).
DRIVER_C_ALL := $(shell find $(DRIVER_DIR) -name '*.c' \
		  ! -path '*/pci/*' ! -path '*/virtio/*' ! -path '*/net/*' \
		  ! -path '*/acpi/*' ! -name 'gui.c' ! -name 'splash_img.S' \
		  2>/dev/null)
TEST_SRCS   := tests/harness/test_main.c \
               tests/harness/mock_arch.c \
               tests/test_state_object.c \
               tests/test_cell_lifecycle.c \
               tests/test_cell_runtime.c \
               tests/test_memplane.c \
               tests/test_engine_registry.c \
               tests/test_scheduler.c \
               tests/test_capability.c \
               tests/test_fb.c \
               tests/test_engine_lifecycle.c \
               tests/test_resource_lease.c \
               tests/test_model_server.c \
               tests/test_posix.c \
               tests/test_tensor.c \
               tests/test_model.c \
               tests/test_tensor_ops.c \
               tests/test_crypto.c \
               tests/test_sshd_crypto.c \
               tests/test_input_routing.c
TEST_BIN    := build/test/anunix_test

test:
	@echo "  Building host-native test binary..."
	@mkdir -p build/test
	$(TEST_CC) $(TEST_CFLAGS) $(TEST_SRCS) $(TEST_CORE) $(DRIVER_C_ALL) $(LIB_C) -o $(TEST_BIN)
	@echo "  Running tests..."
	@$(TEST_BIN)

# --- Python prototype (legacy) ---
proto-install:
	pip install -e ".[all]"

proto-test:
	python -m pytest tests/ -v
