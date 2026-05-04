#!/bin/sh
#
# build-iso.sh — Assemble a bootable x86_64 hybrid ISO.
#
# BIOS boot: SYSLINUX/ISOLINUX + mboot.c32 (multiboot1 wrapper kernel)
# UEFI boot: GRUB EFI binary
#
# Output: build/anunix-x86_64.iso

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${TOOLS_DIR}/.." && pwd)"
GRUB_DIR="${TOOLS_DIR}/grub"
XORRISO="${GRUB_DIR}/bin/xorriso"
if ! "${XORRISO}" --version >/dev/null 2>&1; then
	XORRISO="$(which xorriso 2>/dev/null || true)"
fi
GRUB_EFI_DIR="${GRUB_DIR}/lib/grub/x86_64-efi"
SYSLINUX_DIR="${GRUB_DIR}/share"

KERNEL="${PROJECT_DIR}/build/x86_64/anunix.elf"
KERNEL_MB1="${PROJECT_DIR}/build/x86_64/anunix-qemu.elf"
GRUB_CFG="${PROJECT_DIR}/config/grub.cfg"
ISO_DIR="${PROJECT_DIR}/build/iso-staging"
ISO_OUT="${PROJECT_DIR}/build/anunix-x86_64.iso"

# ---------------------------------------------------------------
# Checks
# ---------------------------------------------------------------
if [ -z "${XORRISO}" ] || [ ! -x "${XORRISO}" ]; then
	echo "ERROR: xorriso not found. Run make iso-deps first." >&2; exit 1
fi
if [ ! -f "${SYSLINUX_DIR}/isolinux.bin" ]; then
	echo "ERROR: isolinux.bin not found. Run make iso-deps first." >&2; exit 1
fi
if [ ! -f "${KERNEL}" ]; then
	echo "ERROR: Kernel not found: ${KERNEL}." >&2; exit 1
fi
if [ ! -f "${KERNEL_MB1}" ]; then
	echo "WARNING: ${KERNEL_MB1} not found — BIOS boot needs this."
fi

echo "=== Building Anunix x86_64 ISO ==="
echo ""

# ---------------------------------------------------------------
# Step 1: Stage ISO filesystem
# ---------------------------------------------------------------
echo ">>> [1/3] Staging ISO filesystem..."
rm -rf "${ISO_DIR}"
mkdir -p "${ISO_DIR}/boot"
mkdir -p "${ISO_DIR}/isolinux"
mkdir -p "${ISO_DIR}/EFI/BOOT"
mkdir -p "${ISO_DIR}/boot/grub/x86_64-efi"

cp "${KERNEL}" "${ISO_DIR}/boot/anunix.elf"
echo "  kernel (ELF64): $(ls -lh "${ISO_DIR}/boot/anunix.elf" | awk {print })"

if [ -f "${KERNEL_MB1}" ]; then
	cp "${KERNEL_MB1}" "${ISO_DIR}/boot/anunix-mb1.elf"
	echo "  kernel (MB1):   $(ls -lh "${ISO_DIR}/boot/anunix-mb1.elf" | awk {print })"
fi

# ISOLINUX files — all must be same syslinux version
cp "${SYSLINUX_DIR}/isolinux.bin"  "${ISO_DIR}/isolinux/"
cp "${SYSLINUX_DIR}/ldlinux.c32"   "${ISO_DIR}/isolinux/"
cp "${SYSLINUX_DIR}/libcom32.c32"  "${ISO_DIR}/isolinux/"
cp "${SYSLINUX_DIR}/libutil.c32"   "${ISO_DIR}/isolinux/"
cp "${SYSLINUX_DIR}/mboot.c32"     "${ISO_DIR}/isolinux/"
# libgpl.c32 provides symbols mboot.c32 depends on (e.g. __vesacon_i915resolution)
if [ -f "${SYSLINUX_DIR}/libgpl.c32" ]; then
	cp "${SYSLINUX_DIR}/libgpl.c32" "${ISO_DIR}/isolinux/"
fi

cat > "${ISO_DIR}/isolinux/isolinux.cfg" << ISOCFG
DEFAULT anunix
PROMPT 0
TIMEOUT 30

LABEL anunix
    MENU LABEL Anunix
    KERNEL mboot.c32
    APPEND /boot/anunix-mb1.elf
ISOCFG

echo "  ISOLINUX: $(ls "${ISO_DIR}/isolinux/" | tr n  )"

# GRUB config + EFI modules
cp "${GRUB_CFG}" "${ISO_DIR}/boot/grub/grub.cfg"
cp "${GRUB_EFI_DIR}"/*.mod "${ISO_DIR}/boot/grub/x86_64-efi/"
cp "${GRUB_EFI_DIR}"/*.lst "${ISO_DIR}/boot/grub/x86_64-efi/" 2>/dev/null || true
echo ""

# ---------------------------------------------------------------
# Step 2: UEFI boot image
# ---------------------------------------------------------------
echo ">>> [2/3] Staging UEFI boot..."
GRUB_EFI_BIN="${ISO_DIR}/EFI/BOOT/BOOTX64.EFI"
if [ -f "${GRUB_DIR}/share/BOOTX64.EFI" ]; then
	cp "${GRUB_DIR}/share/BOOTX64.EFI" "${GRUB_EFI_BIN}"
elif [ -f "${GRUB_EFI_DIR}/monolithic/grubx64.efi" ]; then
	cp "${GRUB_EFI_DIR}/monolithic/grubx64.efi" "${GRUB_EFI_BIN}"
fi

EFI_IMG=""
if [ -f "${GRUB_EFI_BIN}" ]; then
	EFI_IMG="${ISO_DIR}/boot/grub/efiboot.img"
	# Size the FAT image to fit BOOTX64.EFI plus FAT overhead (~1MB).
	EFI_BIN_KB=$(( ($(stat -c %s "${GRUB_EFI_BIN}") + 1023) / 1024 ))
	EFI_IMG_KB=$(( EFI_BIN_KB + 1024 ))
	# Round up to next 1MB boundary, minimum 8MB.
	EFI_IMG_KB=$(( ((EFI_IMG_KB + 1023) / 1024) * 1024 ))
	if [ "${EFI_IMG_KB}" -lt 8192 ]; then EFI_IMG_KB=8192; fi
	dd if=/dev/zero of="${EFI_IMG}" bs=1k count="${EFI_IMG_KB}" 2>/dev/null
	if command -v mformat >/dev/null 2>&1; then
		mformat -i "${EFI_IMG}" -F ::
		mmd   -i "${EFI_IMG}" ::/EFI ::/EFI/BOOT
		mcopy -i "${EFI_IMG}" "${GRUB_EFI_BIN}" ::/EFI/BOOT/BOOTX64.EFI
		echo "  ESP: $(ls -lh "${EFI_IMG}" | awk {print })"
	elif command -v hdiutil >/dev/null 2>&1; then
		EFI_DEV=$(hdiutil attach -nomount "${EFI_IMG}" 2>/dev/null | head -1 | awk {print })
		newfs_msdos -F 12 "${EFI_DEV}" >/dev/null 2>&1
		EFI_MNT="/tmp/efi_mnt_$$"; mkdir -p "${EFI_MNT}"
		mount -t msdos "${EFI_DEV}" "${EFI_MNT}" 2>/dev/null
		mkdir -p "${EFI_MNT}/EFI/BOOT"
		cp "${GRUB_EFI_BIN}" "${EFI_MNT}/EFI/BOOT/BOOTX64.EFI"
		umount "${EFI_MNT}" 2>/dev/null; hdiutil detach "${EFI_DEV}" >/dev/null 2>&1
		rmdir "${EFI_MNT}" 2>/dev/null || true
		echo "  ESP: $(ls -lh "${EFI_IMG}" | awk {print })"
	else
		echo "  WARNING: no mtools/hdiutil — UEFI boot skipped (BIOS works)"
		EFI_IMG=""
	fi
fi
echo ""

# ---------------------------------------------------------------
# Step 3: Assemble hybrid ISO
# ---------------------------------------------------------------
echo ">>> [3/3] Creating hybrid ISO..."

XORRISO_ARGS="-as mkisofs -R -J -V ANUNIX \
	-b isolinux/isolinux.bin \
	-c isolinux/boot.cat \
	-no-emul-boot \
	-boot-load-size 4 \
	-boot-info-table"

if [ -n "${EFI_IMG}" ] && [ -f "${EFI_IMG}" ]; then
	XORRISO_ARGS="${XORRISO_ARGS} \
		-eltorito-alt-boot \
		-e boot/grub/efiboot.img \
		-no-emul-boot \
		-isohybrid-gpt-basdat"
fi

eval "${XORRISO}" ${XORRISO_ARGS} -o "${ISO_OUT}" "${ISO_DIR}" 2>&1 | tail -5
echo ""

if [ -f "${ISO_OUT}" ]; then
	SIZE=$(du -sh "${ISO_OUT}" | cut -f1)
	echo "=== ISO created: ${ISO_OUT} (${SIZE}) ==="
else
	echo "ERROR: ISO creation failed" >&2; exit 1
fi
