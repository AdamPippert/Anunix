#!/bin/sh
#
# fetch-llvm.sh — Download LLVM linker tools for Anunix kernel builds.
#
# Apple's Command Line Tools ship clang with full target support but
# omit ld.lld and llvm-objcopy. This script fetches a pre-built LLVM
# release and extracts only the binaries we need into tools/llvm/bin/.
#
# No Homebrew required. Works on both Intel and Apple Silicon Macs.
# Run once: 'make toolchain'

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
LLVM_DIR="${TOOLS_DIR}/llvm"
BIN_DIR="${LLVM_DIR}/bin"

if [ -x "${BIN_DIR}/ld.lld" ] && [ -x "${BIN_DIR}/llvm-objcopy" ]; then
	echo "LLVM tools already present in ${BIN_DIR}"
	"${BIN_DIR}/ld.lld" --version
	exit 0
fi

# Detect host OS and architecture, pick the right LLVM release.
# We only need ld.lld and llvm-objcopy.
UNAME_M=$(uname -m)
UNAME_S=$(uname -s)

case "${UNAME_S}-${UNAME_M}" in
	Darwin-x86_64)
		LLVM_VERSION="15.0.7"
		ARCHIVE_NAME="clang+llvm-${LLVM_VERSION}-x86_64-apple-darwin21.0"
		;;
	Darwin-arm64)
		LLVM_VERSION="18.1.8"
		ARCHIVE_NAME="clang+llvm-${LLVM_VERSION}-arm64-apple-macos11"
		;;
	Linux-x86_64)
		LLVM_VERSION="18.1.8"
		ARCHIVE_NAME="clang+llvm-${LLVM_VERSION}-x86_64-linux-gnu-ubuntu-18.04"
		;;
	Linux-aarch64)
		LLVM_VERSION="18.1.8"
		ARCHIVE_NAME="clang+llvm-${LLVM_VERSION}-aarch64-linux-gnu"
		;;
	*)
		echo "Unsupported platform: ${UNAME_S}-${UNAME_M}" >&2
		exit 1
		;;
esac

TARBALL="${ARCHIVE_NAME}.tar.xz"
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${TARBALL}"

echo "Host:    ${UNAME_M}"
echo "LLVM:    ${LLVM_VERSION}"
echo "Archive: ${TARBALL}"
echo "URL:     ${URL}"
echo ""

TMPDIR=$(mktemp -d)
trap 'rm -rf "${TMPDIR}"' EXIT

cd "${TMPDIR}"
echo "Downloading (~500-800 MB, this takes a while)..."
curl -L --progress-bar -o "${TARBALL}" "${URL}"

# Verify we got an actual archive, not an error page
FILE_TYPE=$(file -b "${TARBALL}" | head -1)
case "${FILE_TYPE}" in
	*XZ*|*xz*|*data*)
		;;
	*)
		echo "ERROR: Download appears corrupt or is not an archive." >&2
		echo "file reports: ${FILE_TYPE}" >&2
		echo "URL may be wrong. Check: ${URL}" >&2
		exit 1
		;;
esac

echo ""
echo "Extracting ld.lld and llvm-objcopy..."
mkdir -p "${BIN_DIR}"

# Try selective extract first (fast), fall back to full extract
tar xf "${TARBALL}" "${ARCHIVE_NAME}/bin/ld.lld" "${ARCHIVE_NAME}/bin/llvm-objcopy" "${ARCHIVE_NAME}/bin/lld" 2>/dev/null || {
	echo "Selective extract failed, doing full extract (slower)..."
	tar xf "${TARBALL}"
}

for bin in ld.lld llvm-objcopy lld; do
	if [ -f "${ARCHIVE_NAME}/bin/${bin}" ]; then
		cp "${ARCHIVE_NAME}/bin/${bin}" "${BIN_DIR}/"
		chmod +x "${BIN_DIR}/${bin}"
		echo "  installed: ${bin}"
	fi
done

echo ""
"${BIN_DIR}/ld.lld" --version
echo ""
echo "Done. Run 'make toolchain-check' to verify."
