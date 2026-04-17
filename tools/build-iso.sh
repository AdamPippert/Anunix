#!/bin/sh
#
# build-iso.sh — Assemble a bootable x86_64 hybrid ISO.
#
# Creates a BIOS + UEFI bootable ISO using the multiboot kernel
# wrapper and GRUB. Requires: make iso-deps (GRUB modules + xorriso).
#
# Output: build/anunix-x86_64.iso
#

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${TOOLS_DIR}/.." && pwd)"
GRUB_DIR="${TOOLS_DIR}/grub"
XORRISO="${GRUB_DIR}/bin/xorriso"
GRUB_I386="${GRUB_DIR}/lib/grub/i386-pc"
GRUB_EFI="${GRUB_DIR}/lib/grub/x86_64-efi"

KERNEL="${PROJECT_DIR}/build/x86_64/anunix.elf"
KERNEL_MB1="${PROJECT_DIR}/build/x86_64/anunix-qemu.elf"
GRUB_CFG="${PROJECT_DIR}/config/grub.cfg"
SYSLINUX_DIR="${GRUB_DIR}/share"
ISO_DIR="${PROJECT_DIR}/build/iso-staging"
ISO_OUT="${PROJECT_DIR}/build/anunix-x86_64.iso"

# ---------------------------------------------------------------
# Checks
# ---------------------------------------------------------------

if [ ! -x "${XORRISO}" ]; then
	echo "ERROR: xorriso not found. Run 'make iso-deps' first." >&2
	exit 1
fi

if [ ! -d "${GRUB_I386}" ]; then
	echo "ERROR: GRUB i386-pc modules not found. Run 'make iso-deps' first." >&2
	exit 1
fi

if [ ! -d "${GRUB_EFI}" ]; then
	echo "ERROR: GRUB x86_64-efi modules not found. Run 'make iso-deps' first." >&2
	exit 1
fi

if [ ! -f "${KERNEL}" ]; then
	echo "ERROR: Kernel not found at ${KERNEL}. Run 'make kernel ARCH=x86_64' first." >&2
	exit 1
fi

if [ ! -f "${KERNEL_MB1}" ]; then
	echo "WARNING: Legacy BIOS kernel not found at ${KERNEL_MB1}."
	echo "  Legacy BIOS boot will not work. UEFI boot is unaffected."
	echo "  To build it: make ARCH=x86_64 build/x86_64/anunix-qemu.elf"
fi

echo "=== Building Anunix x86_64 ISO ==="
echo ""

# ---------------------------------------------------------------
# Step 1: Create ISO staging directory
# ---------------------------------------------------------------
echo ">>> [1/3] Staging ISO filesystem..."
rm -rf "${ISO_DIR}"
mkdir -p "${ISO_DIR}/boot"
mkdir -p "${ISO_DIR}/isolinux"
mkdir -p "${ISO_DIR}/EFI/BOOT"

# Copy kernels
cp "${KERNEL}" "${ISO_DIR}/boot/anunix.elf"
echo "  kernel (UEFI): $(ls -lh "${ISO_DIR}/boot/anunix.elf" | awk '{print $5}')"

if [ -f "${KERNEL_MB1}" ]; then
	cp "${KERNEL_MB1}" "${ISO_DIR}/boot/anunix-mb1.elf"
	echo "  kernel (BIOS): $(ls -lh "${ISO_DIR}/boot/anunix-mb1.elf" | awk '{print $5}')"
fi

# Copy EFI stub if built (direct UEFI boot without GRUB)
EFI_STUB="${PROJECT_DIR}/build/x86_64/BOOTX64.EFI"
if [ -f "${EFI_STUB}" ]; then
	# Place as a secondary EFI boot option
	cp "${EFI_STUB}" "${ISO_DIR}/EFI/BOOT/ANUNIX.EFI"
	echo "  EFI stub: $(ls -lh "${ISO_DIR}/EFI/BOOT/ANUNIX.EFI" | awk '{print $5}')"
fi

# Set up ISOLINUX (BIOS CD-ROM bootloader)
if [ -f "${SYSLINUX_DIR}/isolinux.bin" ]; then
	cp "${SYSLINUX_DIR}/isolinux.bin" "${ISO_DIR}/isolinux/"
	cp "${SYSLINUX_DIR}/ldlinux.c32" "${ISO_DIR}/isolinux/" 2>/dev/null || true
	cp "${SYSLINUX_DIR}/mboot.c32" "${ISO_DIR}/isolinux/" 2>/dev/null || true
	cp "${SYSLINUX_DIR}/libcom32.c32" "${ISO_DIR}/isolinux/" 2>/dev/null || true
	cp "${SYSLINUX_DIR}/libutil.c32" "${ISO_DIR}/isolinux/" 2>/dev/null || true

	# Create ISOLINUX configuration (BIOS path uses multiboot1 wrapper)
	cat > "${ISO_DIR}/isolinux/isolinux.cfg" <<-'ISOCFG'
		SERIAL 0 115200
		DEFAULT anunix
		PROMPT 1
		TIMEOUT 30

		LABEL anunix
		    MENU LABEL Anunix
		    KERNEL mboot.c32
		    APPEND /boot/anunix-mb1.elf
	ISOCFG

	echo "  ISOLINUX: $(ls "${ISO_DIR}/isolinux/" | tr '\n' ' ')"
else
	echo "  ERROR: isolinux.bin not found. Run 'make iso-deps' first."
	exit 1
fi

echo ""

# ---------------------------------------------------------------
# Step 3: Create EFI boot image
# ---------------------------------------------------------------
echo ">>> [2/3] Creating EFI boot image..."

# Check for pre-built GRUB EFI binary
GRUB_EFI_BIN=""
if [ -f "${GRUB_DIR}/share/BOOTX64.EFI" ]; then
	GRUB_EFI_BIN="${GRUB_DIR}/share/BOOTX64.EFI"
elif [ -f "${GRUB_EFI}/monolithic/grubx64.efi" ]; then
	GRUB_EFI_BIN="${GRUB_EFI}/monolithic/grubx64.efi"
fi

if [ -n "${GRUB_EFI_BIN}" ]; then
	cp "${GRUB_EFI_BIN}" "${ISO_DIR}/EFI/BOOT/BOOTX64.EFI"
else
	# Try to assemble a minimal GRUB EFI binary from modules
	# This requires grub-mkimage which we may not have.
	# Create a placeholder — UEFI boot won't work without it.
	echo "  WARNING: No GRUB EFI binary found — UEFI boot may not work"
	echo "  You can manually place BOOTX64.EFI in tools/grub/share/"
fi

# Copy GRUB config and modules so insmod works at boot time
mkdir -p "${ISO_DIR}/boot/grub/x86_64-efi"
cp "${GRUB_CFG}" "${ISO_DIR}/boot/grub/grub.cfg"
cp "${GRUB_EFI}"/*.mod "${ISO_DIR}/boot/grub/x86_64-efi/"
cp "${GRUB_EFI}"/*.lst "${ISO_DIR}/boot/grub/x86_64-efi/" 2>/dev/null || true
echo "  grub.cfg: /boot/grub/grub.cfg"
echo "  modules: $(ls "${ISO_DIR}/boot/grub/x86_64-efi/"*.mod | wc -l | tr -d ' ') modules"

# Create EFI system partition image (raw FAT12, 4 MB)
EFI_IMG="${ISO_DIR}/EFI/BOOT/efiboot.img"

if [ -f "${ISO_DIR}/EFI/BOOT/BOOTX64.EFI" ]; then
	# Create a raw FAT image using macOS-native tools (no mtools/Homebrew)
	EFI_MNT="${BUILD_TMP:-/tmp}/efi_mount_$$"

	# 1. Create blank 8 MB raw image (must fit BOOTX64.EFI + grub.cfg + FAT overhead)
	dd if=/dev/zero of="${EFI_IMG}" bs=1k count=8192 2>/dev/null

	# 2. Attach as a block device without mounting
	EFI_DEV=$(hdiutil attach -nomount "${EFI_IMG}" 2>/dev/null \
		| head -1 | awk '{print $1}')

	if [ -z "${EFI_DEV}" ]; then
		echo "  ERROR: Could not attach EFI image" >&2
		exit 1
	fi

	# 3. Format as FAT12
	newfs_msdos -F 12 "${EFI_DEV}" > /dev/null 2>&1

	# 4. Mount, copy files, unmount
	mkdir -p "${EFI_MNT}"
	mount -t msdos "${EFI_DEV}" "${EFI_MNT}" 2>/dev/null

	mkdir -p "${EFI_MNT}/EFI/BOOT"
	cp "${ISO_DIR}/EFI/BOOT/BOOTX64.EFI" "${EFI_MNT}/EFI/BOOT/"
	# Include grub.cfg inside EFI image as fallback
	mkdir -p "${EFI_MNT}/boot/grub"
	cp "${GRUB_CFG}" "${EFI_MNT}/boot/grub/grub.cfg"

	umount "${EFI_MNT}" 2>/dev/null
	hdiutil detach "${EFI_DEV}" > /dev/null 2>&1
	rmdir "${EFI_MNT}" 2>/dev/null || true

	echo "  efiboot.img: $(ls -lh "${EFI_IMG}" | awk '{print $5}')"
else
	echo "  Skipping EFI boot image (no BOOTX64.EFI available)"
	echo "  (BIOS boot will still work)"
fi

echo ""

# ---------------------------------------------------------------
# Step 4: Assemble ISO with xorriso
# ---------------------------------------------------------------
echo ">>> [3/3] Creating hybrid ISO..."

XORRISO_ARGS="-as mkisofs -R -J -V ANUNIX"

# BIOS El Torito boot via ISOLINUX
if [ -f "${ISO_DIR}/isolinux/isolinux.bin" ]; then
	XORRISO_ARGS="${XORRISO_ARGS} \
		-b isolinux/isolinux.bin \
		-c isolinux/boot.cat \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table"
fi

# UEFI El Torito boot
if [ -f "${EFI_IMG}" ]; then
	XORRISO_ARGS="${XORRISO_ARGS} \
		-eltorito-alt-boot \
		-e EFI/BOOT/efiboot.img \
		-no-emul-boot \
		-isohybrid-gpt-basdat"
fi

# Build the ISO
eval "${XORRISO}" ${XORRISO_ARGS} \
	-o "${ISO_OUT}" \
	"${ISO_DIR}" 2>&1 | tail -5

echo ""

if [ -f "${ISO_OUT}" ]; then
	ISO_SIZE=$(ls -lh "${ISO_OUT}" | awk '{print $5}')
	echo "=== ISO created: ${ISO_OUT} (${ISO_SIZE}) ==="
	echo ""
	echo "Boot with QEMU:"
	echo "  qemu-system-x86_64 -m 512M -cdrom ${ISO_OUT} -serial mon:stdio"
	echo ""
	echo "Write to USB drive:"
	echo "  dd if=${ISO_OUT} of=/dev/sdX bs=4M status=progress"
else
	echo "ERROR: ISO creation failed" >&2
	exit 1
fi
