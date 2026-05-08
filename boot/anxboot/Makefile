# Makefile — anxboot.efi (Anunix UEFI loader)
#
# Builds a PE/COFF UEFI application via clang -target x86_64-unknown-windows
# linked with lld-link as -subsystem:efi_application. No GNU EFI, no edk2.

PROJECT_DIR := $(abspath $(CURDIR)/../..)
BUILD_DIR   := $(PROJECT_DIR)/build/anxboot

CC      := clang
LD      := lld-link
OBJCOPY := llvm-objcopy

# UEFI x86_64 PE/COFF target. -mno-red-zone because UEFI doesn't preserve it
# across calls. -fshort-wchar so L"..." gives 16-bit chars (UCS-2). Freestanding,
# no libc, no SSE (UEFI ABI doesn't save XMM regs).
CFLAGS  := -target x86_64-unknown-windows \
           -ffreestanding -fno-stack-protector -fno-stack-check \
           -fshort-wchar -mno-red-zone \
           -mno-sse -mno-mmx \
           -Wall -Wextra -Werror -O2 -g \
           -I.

# UEFI 2.x application linker flags. /entry:efi_main is our entry point.
LDFLAGS := /subsystem:efi_application \
           /entry:efi_main \
           /nodefaultlib \
           /dll \
           /machine:x64

OBJS := $(BUILD_DIR)/anxboot.o $(BUILD_DIR)/handoff.o

ANXBOOT := $(BUILD_DIR)/anxboot.efi

all: $(ANXBOOT)

$(ANXBOOT): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) /out:$@ $(OBJS)
	@echo "  BUILT   $@"

$(BUILD_DIR)/anxboot.o: anxboot.c efi.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c anxboot.c -o $@

$(BUILD_DIR)/handoff.o: handoff.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -target x86_64-unknown-windows -ffreestanding -c handoff.S -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
