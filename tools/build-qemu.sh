#!/bin/sh
#
# build-qemu.sh — Build QEMU and dependencies from source.
#
# On macOS: builds everything from source (no Homebrew required).
# On Linux: recommends using the system package manager instead.
# Run once: 'make qemu-deps'
#
# Dependencies built (in order):
#   1. meson + ninja  (via pip, in a local venv)
#   2. pkg-config     (from source)
#   3. pcre2          (from source — glib needs it)
#   4. glib           (from source — QEMU's main dependency)
#   5. pixman         (from source — pixel manipulation)
#   6. QEMU           (from source — system emulation only)

set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="${TOOLS_DIR}/qemu"
PREFIX="${QEMU_DIR}/prefix"
BIN_DIR="${QEMU_DIR}/bin"
BUILD_TMP="${QEMU_DIR}/build-tmp"
VENV_DIR="${QEMU_DIR}/venv"

# Versions
PKGCONFIG_VER="0.29.2"
PCRE2_VER="10.42"
GLIB_VER="2.78.4"
GLIB_MAJOR="2.78"
PIXMAN_VER="0.42.2"
QEMU_VER="9.0.2"

# Parallelism
NJOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# On Linux, recommend system QEMU instead of building from source
if [ "$(uname -s)" = "Linux" ]; then
	if command -v qemu-system-x86_64 >/dev/null 2>&1; then
		echo "QEMU already available from system package manager."
		echo "  $(qemu-system-x86_64 --version | head -1)"
		echo "Skipping source build. To force: remove this check."
		exit 0
	else
		echo "Linux detected. Install QEMU from your package manager:"
		echo "  Arch:   sudo pacman -S qemu-full"
		echo "  Debian: sudo apt install qemu-system-x86"
		echo "  Fedora: sudo dnf install qemu-system-x86"
		exit 1
	fi
fi

# SDK path for system headers/libs (macOS)
SDK_PATH=$(xcrun --show-sdk-path 2>/dev/null || echo "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")

# Check if already built
if [ -x "${BIN_DIR}/qemu-system-aarch64" ] && [ -x "${BIN_DIR}/qemu-system-x86_64" ]; then
	echo "QEMU already built in ${BIN_DIR}"
	"${BIN_DIR}/qemu-system-aarch64" --version
	exit 0
fi

echo "=== Building QEMU ${QEMU_VER} from source ==="
echo "  prefix:    ${PREFIX}"
echo "  jobs:      ${NJOBS}"
echo "  SDK:       ${SDK_PATH}"
echo ""

mkdir -p "${PREFIX}/bin" "${PREFIX}/lib" "${PREFIX}/include"
mkdir -p "${BIN_DIR}"
mkdir -p "${BUILD_TMP}"

export PATH="${PREFIX}/bin:${VENV_DIR}/bin:${PATH}"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"
export CFLAGS="-I${PREFIX}/include -isysroot ${SDK_PATH}"
export LDFLAGS="-L${PREFIX}/lib"

# ---------------------------------------------------------------
# Step 1: Python venv with meson + ninja
# ---------------------------------------------------------------
echo ">>> [1/6] Setting up Python venv with meson + ninja..."
if [ ! -x "${VENV_DIR}/bin/meson" ]; then
	python3 -m venv "${VENV_DIR}"
	"${VENV_DIR}/bin/pip" install --quiet meson ninja
fi
echo "  meson: $("${VENV_DIR}/bin/meson" --version)"
echo "  ninja: $("${VENV_DIR}/bin/ninja" --version)"
echo ""

# ---------------------------------------------------------------
# Step 2: pkg-config
# ---------------------------------------------------------------
echo ">>> [2/6] Building pkg-config ${PKGCONFIG_VER}..."
if [ ! -x "${PREFIX}/bin/pkg-config" ]; then
	cd "${BUILD_TMP}"
	PKGCONFIG_TAR="pkg-config-${PKGCONFIG_VER}.tar.gz"
	if [ ! -f "${PKGCONFIG_TAR}" ]; then
		curl -L --progress-bar -o "${PKGCONFIG_TAR}" \
			"https://pkgconfig.freedesktop.org/releases/${PKGCONFIG_TAR}"
	fi
	rm -rf "pkg-config-${PKGCONFIG_VER}"
	tar xf "${PKGCONFIG_TAR}"
	cd "pkg-config-${PKGCONFIG_VER}"
	# Internal glib needs -Wno-int-conversion for modern clang
	CFLAGS="${CFLAGS} -Wno-int-conversion" \
		./configure --prefix="${PREFIX}" --with-internal-glib --quiet
	make -j"${NJOBS}" > /dev/null 2>&1
	make install > /dev/null 2>&1
	echo "  installed: pkg-config $("${PREFIX}/bin/pkg-config" --version)"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 3: pcre2
# ---------------------------------------------------------------
echo ">>> [3/6] Building pcre2 ${PCRE2_VER}..."
if ! "${PREFIX}/bin/pkg-config" --exists libpcre2-8 2>/dev/null; then
	cd "${BUILD_TMP}"
	PCRE2_TAR="pcre2-${PCRE2_VER}.tar.gz"
	if [ ! -f "${PCRE2_TAR}" ]; then
		curl -L --progress-bar -o "${PCRE2_TAR}" \
			"https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VER}/${PCRE2_TAR}"
	fi
	rm -rf "pcre2-${PCRE2_VER}"
	tar xf "${PCRE2_TAR}"
	cd "pcre2-${PCRE2_VER}"
	./configure --prefix="${PREFIX}" --enable-shared --disable-static --quiet
	make -j"${NJOBS}" > /dev/null 2>&1
	make install > /dev/null 2>&1
	echo "  installed: pcre2 $("${PREFIX}/bin/pcre2-config" --version)"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 4: glib
# ---------------------------------------------------------------
echo ">>> [4/6] Building glib ${GLIB_VER}..."
if ! "${PREFIX}/bin/pkg-config" --exists glib-2.0 2>/dev/null; then
	cd "${BUILD_TMP}"
	GLIB_TAR="glib-${GLIB_VER}.tar.xz"
	if [ ! -f "${GLIB_TAR}" ]; then
		curl -L --progress-bar -o "${GLIB_TAR}" \
			"https://download.gnome.org/sources/glib/${GLIB_MAJOR}/${GLIB_TAR}"
	fi
	rm -rf "glib-${GLIB_VER}"
	tar xf "${GLIB_TAR}"
	cd "glib-${GLIB_VER}"

	# glib needs libffi — use the one from the macOS SDK
	FFI_PC="${BUILD_TMP}/ffi-shim.pc"
	cat > "${FFI_PC}" <<-FFIEOF
		prefix=${SDK_PATH}/usr
		libdir=\${prefix}/lib
		includedir=\${prefix}/include/ffi
		Name: libffi
		Description: libffi from macOS SDK
		Version: 3.4.0
		Libs: -lffi
		Cflags: -I\${includedir}
	FFIEOF
	export PKG_CONFIG_PATH="${BUILD_TMP}:${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"

	"${VENV_DIR}/bin/meson" setup _build \
		--prefix="${PREFIX}" \
		--default-library=shared \
		-Dnls=disabled \
		-Dtests=false \
		-Dglib_debug=disabled \
		-Ddtrace=false \
		> /dev/null 2>&1
	"${VENV_DIR}/bin/ninja" -C _build -j"${NJOBS}" > /dev/null 2>&1
	"${VENV_DIR}/bin/ninja" -C _build install > /dev/null 2>&1
	echo "  installed: glib $("${PREFIX}/bin/pkg-config" --modversion glib-2.0)"

	# Reset PKG_CONFIG_PATH
	export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 5: pixman
# ---------------------------------------------------------------
echo ">>> [5/6] Building pixman ${PIXMAN_VER}..."
if ! "${PREFIX}/bin/pkg-config" --exists pixman-1 2>/dev/null; then
	cd "${BUILD_TMP}"
	PIXMAN_TAR="pixman-${PIXMAN_VER}.tar.gz"
	if [ ! -f "${PIXMAN_TAR}" ]; then
		curl -L --progress-bar -o "${PIXMAN_TAR}" \
			"https://cairographics.org/releases/${PIXMAN_TAR}"
	fi
	rm -rf "pixman-${PIXMAN_VER}"
	tar xf "${PIXMAN_TAR}"
	cd "pixman-${PIXMAN_VER}"
	"${VENV_DIR}/bin/meson" setup _build \
		--prefix="${PREFIX}" \
		--default-library=shared \
		-Dtests=disabled \
		> /dev/null 2>&1
	"${VENV_DIR}/bin/ninja" -C _build -j"${NJOBS}" > /dev/null 2>&1
	"${VENV_DIR}/bin/ninja" -C _build install > /dev/null 2>&1
	echo "  installed: pixman $("${PREFIX}/bin/pkg-config" --modversion pixman-1)"
else
	echo "  already installed"
fi
echo ""

# ---------------------------------------------------------------
# Step 6: QEMU
# ---------------------------------------------------------------
echo ">>> [6/6] Building QEMU ${QEMU_VER}..."
cd "${BUILD_TMP}"
QEMU_TAR="qemu-${QEMU_VER}.tar.xz"
if [ ! -f "${QEMU_TAR}" ]; then
	curl -L --progress-bar -o "${QEMU_TAR}" \
		"https://download.qemu.org/${QEMU_TAR}"
fi
rm -rf "qemu-${QEMU_VER}"
tar xf "${QEMU_TAR}"
cd "qemu-${QEMU_VER}"

# Detect if we can use Hypervisor.framework (Apple Silicon host)
HVF_FLAG=""
UNAME_M=$(uname -m)
if [ "${UNAME_M}" = "arm64" ]; then
	HVF_FLAG="--enable-hvf"
fi

./configure \
	--prefix="${PREFIX}" \
	--target-list=aarch64-softmmu,x86_64-softmmu \
	--disable-sdl \
	--disable-gtk \
	--disable-opengl \
	--disable-virglrenderer \
	--disable-brlapi \
	--disable-curl \
	--disable-vnc \
	--disable-docs \
	--disable-vte \
	--disable-spice \
	--disable-usb-redir \
	--disable-libusb \
	--disable-capstone \
	${HVF_FLAG} \
	--extra-cflags="-I${PREFIX}/include" \
	--extra-ldflags="-L${PREFIX}/lib" \
	> /dev/null 2>&1

echo "  configuring... done"
echo "  compiling (this takes a few minutes)..."
make -j"${NJOBS}" > /dev/null 2>&1
make install > /dev/null 2>&1

# Symlink QEMU binaries into tools/qemu/bin/ for easy access
for bin in qemu-system-aarch64 qemu-system-x86_64; do
	if [ -x "${PREFIX}/bin/${bin}" ]; then
		ln -sf "${PREFIX}/bin/${bin}" "${BIN_DIR}/${bin}"
		echo "  installed: ${bin}"
	fi
done

echo ""
echo "=== QEMU build complete ==="
"${BIN_DIR}/qemu-system-aarch64" --version
echo ""
echo "Binaries: ${BIN_DIR}/"
echo ""

# Clean up build sources (keep tarballs for rebuild)
echo "Cleaning up build sources..."
cd "${BUILD_TMP}"
rm -rf "pkg-config-${PKGCONFIG_VER}" "pcre2-${PCRE2_VER}" \
       "glib-${GLIB_VER}" "pixman-${PIXMAN_VER}" "qemu-${QEMU_VER}"
echo "Done. Run 'make qemu' to boot the kernel."
