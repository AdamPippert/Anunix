#!/bin/sh
#
# build-uefi-usb.sh — Build a flat UEFI-only USB image for Anunix.
#
# Layout:
#   GPT partition table
#     part 1: ESP, FAT32, 128 MiB
#       /EFI/BOOT/BOOTX64.EFI    GRUB EFI (monolithic)
#       /boot/anunix.elf         Anunix kernel
#       /boot/grub/grub.cfg      multiboot2 entry for the kernel
#
# This intentionally drops the ISO9660 + isolinux + El Torito hybrid path —
# Framework UEFI (and most modern UEFI firmware) only honors a real GPT ESP
# anyway, so the hybrid layer was just adding failure modes.
#
# Output: build/anunix-x86_64-uefi.img
#         dd it straight to a USB stick.

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${TOOLS_DIR}/.." && pwd)"
GRUB_DIR="${TOOLS_DIR}/grub"
GRUB_EFI_DIR="${GRUB_DIR}/lib/grub/x86_64-efi"

KERNEL="${PROJECT_DIR}/build/x86_64/anunix.elf"
KERNEL_MB1="${PROJECT_DIR}/build/x86_64/anunix-qemu.elf"
IMG_OUT="${PROJECT_DIR}/build/anunix-x86_64-uefi.img"

ESP_MB=128					# total ESP size, MiB
TOTAL_MB=$((ESP_MB + 2))			# +2 MiB for GPT + slack
IMG_BYTES=$((TOTAL_MB * 1024 * 1024))
ESP_BYTES=$((ESP_MB * 1024 * 1024))

# Sector arithmetic — 512 B sectors.
ESP_START_SEC=2048				# 1 MiB align
ESP_SECTORS=$((ESP_BYTES / 512))
ESP_END_SEC=$((ESP_START_SEC + ESP_SECTORS - 1))

# ---------------------------------------------------------------
echo "=== Building Anunix UEFI USB image ==="
echo ""

if [ ! -f "${KERNEL}" ]; then
	echo "ERROR: kernel not found: ${KERNEL}" >&2
	echo "  Run: make kernel ARCH=x86_64" >&2
	exit 1
fi

ANXBOOT_EFI="${PROJECT_DIR}/build/anxboot/anxboot.efi"
EFI_BIN_SRC=""
USE_ANXBOOT=0
if [ -f "${ANXBOOT_EFI}" ]; then
	EFI_BIN_SRC="${ANXBOOT_EFI}"
	USE_ANXBOOT=1
elif [ -f "${GRUB_EFI_DIR}/monolithic/grubx64.efi" ]; then
	EFI_BIN_SRC="${GRUB_EFI_DIR}/monolithic/grubx64.efi"
elif [ -f "${GRUB_DIR}/share/BOOTX64.EFI" ]; then
	EFI_BIN_SRC="${GRUB_DIR}/share/BOOTX64.EFI"
else
	echo "ERROR: no EFI loader found." >&2
	echo "  Build anxboot:  make -C boot/anxboot" >&2
	echo "  Or fetch GRUB:  make iso-deps" >&2
	exit 1
fi

if ! command -v sfdisk >/dev/null 2>&1; then
	echo "ERROR: sfdisk not found (util-linux)" >&2; exit 1
fi
if ! command -v mformat >/dev/null 2>&1; then
	echo "ERROR: mformat not found (mtools)" >&2; exit 1
fi

# ---------------------------------------------------------------
# 1. Allocate image and write GPT
# ---------------------------------------------------------------
echo ">>> [1/4] Allocate image (${TOTAL_MB} MiB) and write GPT..."
rm -f "${IMG_OUT}"
truncate -s "${IMG_BYTES}" "${IMG_OUT}"

# ESP type GUID is C12A7328-F81F-11D2-BA4B-00A0C93EC93B.
sfdisk --no-reread --no-tell-kernel --label gpt "${IMG_OUT}" >/dev/null <<EOF
label: gpt
unit: sectors

start=${ESP_START_SEC}, size=${ESP_SECTORS}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI System"
EOF
echo "  ESP: sector ${ESP_START_SEC}..${ESP_END_SEC} (${ESP_MB} MiB, type C12A7328-...)"

# ---------------------------------------------------------------
# 2. Stage ESP contents into a sibling FAT image
# ---------------------------------------------------------------
echo ">>> [2/4] Format ESP as FAT32 and stage files..."
ESP_IMG="$(mktemp -u "${PROJECT_DIR}/build/esp-XXXXXX.img")"
truncate -s "${ESP_BYTES}" "${ESP_IMG}"

# FAT32 requires >=33 MiB; we use 128 MiB so it's safe.
mformat -i "${ESP_IMG}" -F -v ANXBOOT ::

mmd  -i "${ESP_IMG}" ::/EFI ::/EFI/BOOT ::/boot
mcopy -i "${ESP_IMG}" "${EFI_BIN_SRC}" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "${ESP_IMG}" "${KERNEL}"      ::/boot/anunix.elf
if [ -f "${KERNEL_MB1}" ]; then
	mcopy -i "${ESP_IMG}" "${KERNEL_MB1}" ::/boot/anunix-mb1.elf
fi

# anxboot doesn't need grub.cfg or any helper modules — it reads
# /boot/anunix.elf directly. GRUB-based boot needs both.
if [ "${USE_ANXBOOT}" -eq 0 ]; then
	mmd -i "${ESP_IMG}" ::/boot/grub ::/boot/grub/x86_64-efi
	mcopy -i "${ESP_IMG}" -b "${GRUB_EFI_DIR}"/*.mod "${GRUB_EFI_DIR}"/*.lst \
		::/boot/grub/x86_64-efi/ 2>/dev/null
fi

if [ "${USE_ANXBOOT}" -eq 1 ]; then
	echo "  /EFI/BOOT/BOOTX64.EFI       anxboot 0.1 ($(stat -c %s "${EFI_BIN_SRC}") bytes)"
	echo "  /boot/anunix.elf            $(stat -c %s "${KERNEL}") bytes"
	if [ -f "${KERNEL_MB1}" ]; then
		echo "  /boot/anunix-mb1.elf        $(stat -c %s "${KERNEL_MB1}") bytes"
	fi
	echo ">>> [3/4] Splice ESP into disk image at sector ${ESP_START_SEC}..."
	dd if="${ESP_IMG}" of="${IMG_OUT}" \
	   bs=512 seek="${ESP_START_SEC}" conv=notrunc status=none
	rm -f "${ESP_IMG}"
	echo ">>> [4/4] Verify..."
	sfdisk -l "${IMG_OUT}" 2>/dev/null | grep -E '^Disk |^Disklabel|^Device' | head -5
	echo ""
	SIZE=$(du -sh "${IMG_OUT}" | cut -f1)
	echo "=== UEFI USB image: ${IMG_OUT} (${SIZE}) ==="
	echo "    dd if=${IMG_OUT} of=/dev/sdX bs=4M oflag=sync"
	exit 0
fi

# Fresh grub.cfg — UEFI path uses multiboot2 directly (no ANUNIX.EFI stub).
GRUB_CFG_TMP="$(mktemp)"
cat > "${GRUB_CFG_TMP}" <<'GRUBCFG'
# Anunix UEFI USB — direct multiboot1 boot.
#
# Framework UEFI (and many other modern UEFIs) expose a very narrow set
# of GOP video modes — sometimes only the native panel resolution and
# nothing else. GRUB's multiboot loader calls grub_video_set_mode during
# the handoff and dies with "no suitable video mode found" when its
# preferred mode list is empty. Forcing gfxpayload=text tells GRUB to
# skip that probe and hand control to the kernel in firmware text mode;
# the kernel re-acquires the framebuffer itself via its own GOP probe.
set timeout=3
set default=0
set gfxpayload=text
set gfxmode=text

# Stay on the firmware text console; do not load gfxterm.
terminal_input console
terminal_output console

insmod multiboot
insmod multiboot2

menuentry "Anunix" {
    set gfxpayload=text
    echo "Loading kernel via multiboot1 (QEMU wrapper)..."
    multiboot /boot/anunix-mb1.elf
    echo "Booting..."
    boot
}

menuentry "Anunix (multiboot2)" {
    set gfxpayload=text
    echo "Loading kernel via multiboot2..."
    multiboot2 /boot/anunix.elf
    echo "Booting..."
    boot
}

menuentry "Anunix (install)" {
    set gfxpayload=text
    multiboot /boot/anunix-mb1.elf -- install
    boot
}
GRUBCFG
mcopy -i "${ESP_IMG}" "${GRUB_CFG_TMP}" ::/boot/grub/grub.cfg
rm -f "${GRUB_CFG_TMP}"

echo "  /EFI/BOOT/BOOTX64.EFI       $(stat -c %s "${EFI_BIN_SRC}") bytes"
echo "  /boot/anunix.elf            $(stat -c %s "${KERNEL}") bytes"
echo "  /boot/grub/x86_64-efi/      $(ls "${GRUB_EFI_DIR}"/*.mod | wc -l) modules"
echo "  /boot/grub/grub.cfg         multiboot2 entry"

# ---------------------------------------------------------------
# 3. Splice ESP into the disk image at the partition offset
# ---------------------------------------------------------------
echo ">>> [3/4] Splice ESP into disk image at sector ${ESP_START_SEC}..."
dd if="${ESP_IMG}" of="${IMG_OUT}" \
   bs=512 seek="${ESP_START_SEC}" conv=notrunc status=none
rm -f "${ESP_IMG}"

# ---------------------------------------------------------------
# 4. Verify
# ---------------------------------------------------------------
echo ">>> [4/4] Verify..."
sfdisk -l "${IMG_OUT}" 2>/dev/null | grep -E '^Disk |^Disklabel|^Device' | head -5
echo ""
SIZE=$(du -sh "${IMG_OUT}" | cut -f1)
echo "=== UEFI USB image: ${IMG_OUT} (${SIZE}) ==="
echo "    dd if=${IMG_OUT} of=/dev/sdX bs=4M oflag=sync"
