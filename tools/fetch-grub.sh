#!/bin/sh
#
# fetch-grub.sh — Download GRUB modules and build xorriso.
#
# Downloads pre-built GRUB BIOS and EFI modules from Debian packages,
# and builds xorriso from source. No Homebrew required.
#
# Run once: 'make iso-deps'
#

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
GRUB_DIR="${TOOLS_DIR}/grub"
BIN_DIR="${GRUB_DIR}/bin"
LIB_DIR="${GRUB_DIR}/lib/grub"
SHARE_DIR="${GRUB_DIR}/share"
BUILD_TMP="${GRUB_DIR}/build-tmp"

NJOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Debian package versions (bookworm/stable)
GRUB_VER="2.06-13+deb12u1"
GRUB_PC_DEB="grub-pc-bin_${GRUB_VER}_amd64.deb"
GRUB_EFI_DEB="grub-efi-amd64-bin_${GRUB_VER}_amd64.deb"
DEBIAN_MIRROR="https://deb.debian.org/debian/pool/main/g/grub2"

# SYSLINUX (provides isolinux.bin + mboot.c32 for CD boot)
SYSLINUX_VER="6.03"
SYSLINUX_TAR="syslinux-${SYSLINUX_VER}.tar.xz"
SYSLINUX_URL="https://mirrors.edge.kernel.org/pub/linux/utils/boot/syslinux/${SYSLINUX_TAR}"

# xorriso version
XORRISO_VER="1.5.6"
XORRISO_TAR="xorriso-${XORRISO_VER}.pl02.tar.gz"
XORRISO_URL="https://www.gnu.org/software/xorriso/${XORRISO_TAR}"

# OVMF (UEFI firmware for QEMU testing)
OVMF_URL="https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd"

# Check if already set up
if [ -x "${BIN_DIR}/xorriso" ] && [ -d "${LIB_DIR}/i386-pc" ] && [ -d "${LIB_DIR}/x86_64-efi" ] && [ -f "${SHARE_DIR}/isolinux.bin" ]; then
	echo "ISO build tools already present in ${GRUB_DIR}"
	exit 0
fi

echo "=== Setting up ISO build tools ==="
echo ""

mkdir -p "${BIN_DIR}" "${LIB_DIR}" "${SHARE_DIR}" "${BUILD_TMP}"

# ---------------------------------------------------------------
# Step 1: Download and extract GRUB BIOS modules
# ---------------------------------------------------------------
echo ">>> [1/4] Downloading GRUB BIOS modules..."
if [ ! -d "${LIB_DIR}/i386-pc" ]; then
	cd "${BUILD_TMP}"
	if [ ! -f "${GRUB_PC_DEB}" ]; then
		curl -L --progress-bar -o "${GRUB_PC_DEB}" \
			"${DEBIAN_MIRROR}/${GRUB_PC_DEB}" || {
			echo "  Trying alternative mirror..."
			curl -L --progress-bar -o "${GRUB_PC_DEB}" \
				"https://ftp.debian.org/debian/pool/main/g/grub2/${GRUB_PC_DEB}"
		}
	fi

	# Extract .deb (ar archive containing data.tar.xz)
	mkdir -p grub-pc-extract
	cd grub-pc-extract
	ar x "../${GRUB_PC_DEB}"
	tar xf data.tar.* 2>/dev/null || tar xf data.tar 2>/dev/null

	cp -r usr/lib/grub/i386-pc "${LIB_DIR}/"
	cd "${BUILD_TMP}"
	rm -rf grub-pc-extract
	echo "  installed: i386-pc modules"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 2: Download and extract GRUB EFI modules
# ---------------------------------------------------------------
echo ">>> [2/4] Downloading GRUB EFI modules..."
if [ ! -d "${LIB_DIR}/x86_64-efi" ]; then
	cd "${BUILD_TMP}"
	if [ ! -f "${GRUB_EFI_DEB}" ]; then
		curl -L --progress-bar -o "${GRUB_EFI_DEB}" \
			"${DEBIAN_MIRROR}/${GRUB_EFI_DEB}" || {
			echo "  Trying alternative mirror..."
			curl -L --progress-bar -o "${GRUB_EFI_DEB}" \
				"https://ftp.debian.org/debian/pool/main/g/grub2/${GRUB_EFI_DEB}"
		}
	fi

	mkdir -p grub-efi-extract
	cd grub-efi-extract
	ar x "../${GRUB_EFI_DEB}"
	tar xf data.tar.* 2>/dev/null || tar xf data.tar 2>/dev/null

	cp -r usr/lib/grub/x86_64-efi "${LIB_DIR}/"
	cd "${BUILD_TMP}"
	rm -rf grub-efi-extract
	echo "  installed: x86_64-efi modules"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 3: Download SYSLINUX (isolinux + mboot.c32)
# ---------------------------------------------------------------
echo ">>> [3/5] Downloading SYSLINUX ${SYSLINUX_VER}..."
if [ ! -f "${SHARE_DIR}/isolinux.bin" ]; then
	cd "${BUILD_TMP}"
	if [ ! -f "${SYSLINUX_TAR}" ]; then
		curl -L --progress-bar -o "${SYSLINUX_TAR}" "${SYSLINUX_URL}"
	fi

	tar xf "${SYSLINUX_TAR}" --include="*/bios/core/isolinux.bin" \
		--include="*/bios/com32/mboot/mboot.c32" \
		--include="*/bios/com32/lib/libcom32.c32" \
		--include="*/bios/com32/elflink/ldlinux/ldlinux.c32" \
		--include="*/bios/com32/libutil/libutil.c32" \
		--include="*/bios/com32/gpl/libgpl.c32" \
		2>/dev/null || tar xf "${SYSLINUX_TAR}"

	# Find and copy the needed files
	find "syslinux-${SYSLINUX_VER}" -name "isolinux.bin" -path "*/bios/*" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/"
	find "syslinux-${SYSLINUX_VER}" -name "mboot.c32" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/"
	find "syslinux-${SYSLINUX_VER}" -name "ldlinux.c32" -path "*/bios/*" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/"
	find "syslinux-${SYSLINUX_VER}" -name "libcom32.c32" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/"
	find "syslinux-${SYSLINUX_VER}" -name "libutil.c32" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/"
	find "syslinux-${SYSLINUX_VER}" -name "libgpl.c32" | head -1 | \
		xargs -I{} cp {} "${SHARE_DIR}/" 2>/dev/null || true

	rm -rf "syslinux-${SYSLINUX_VER}"

	if [ -f "${SHARE_DIR}/isolinux.bin" ]; then
		echo "  installed: isolinux.bin, mboot.c32"
	else
		echo "  WARNING: SYSLINUX extraction failed"
	fi
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 4: Build xorriso from source
# ---------------------------------------------------------------
echo ">>> [4/5] Building xorriso ${XORRISO_VER}..."
if [ ! -x "${BIN_DIR}/xorriso" ]; then
	cd "${BUILD_TMP}"
	if [ ! -f "${XORRISO_TAR}" ]; then
		curl -L --progress-bar -o "${XORRISO_TAR}" "${XORRISO_URL}"
	fi

	XORRISO_SRC="xorriso-${XORRISO_VER}"
	rm -rf "${XORRISO_SRC}"
	tar xf "${XORRISO_TAR}"
	cd "${XORRISO_SRC}"

	./configure --prefix="${GRUB_DIR}" --quiet 2>&1 | tail -3
	make -j"${NJOBS}" > /dev/null 2>&1
	make install > /dev/null 2>&1

	echo "  installed: xorriso $("${BIN_DIR}/xorriso" --version 2>&1 | head -1)"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 4: Download OVMF firmware for UEFI testing
# ---------------------------------------------------------------
echo ">>> [5/5] Downloading OVMF firmware..."
if [ ! -f "${SHARE_DIR}/OVMF.fd" ]; then
	curl -L --progress-bar -o "${SHARE_DIR}/OVMF.fd" "${OVMF_URL}" || {
		echo "  OVMF download failed (optional — UEFI testing will be unavailable)"
	}
	if [ -f "${SHARE_DIR}/OVMF.fd" ]; then
		echo "  installed: OVMF.fd"
	fi
else
	echo "  already installed"
fi
echo ""

# Clean up build sources (keep tarballs for rebuild)
echo "Cleaning up..."
cd "${BUILD_TMP}"
rm -rf "xorriso-${XORRISO_VER}"

echo ""
echo "=== ISO build tools ready ==="
echo "  xorriso:  ${BIN_DIR}/xorriso"
echo "  GRUB:     ${LIB_DIR}/{i386-pc,x86_64-efi}/"
echo ""
echo "Run 'make iso' to build a bootable ISO."
