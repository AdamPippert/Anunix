#!/bin/sh
# run.sh — Launch Anunix in QEMU with sane defaults
# Usage: ./run.sh [--arch x86_64|arm64] [--mem 512M|1G|...] [--fb]
#
# Environment overrides:
#   ANX_MEM=1G      guest RAM (default: 512M)
#   ANX_ARCH=arm64  target architecture (default: host arch)
#   ANX_FB=1        enable framebuffer display (default: serial only)

set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
VERSION="2026.5.8"

# ── Parse args ──────────────────────────────────────────────────────────────
ANX_MEM="${ANX_MEM:-512M}"
ANX_FB="${ANX_FB:-0}"

case "$(uname -m)" in
  arm64|aarch64) ANX_ARCH="${ANX_ARCH:-arm64}" ;;
  *)             ANX_ARCH="${ANX_ARCH:-x86_64}" ;;
esac

while [ $# -gt 0 ]; do
  case "$1" in
    --arch)  ANX_ARCH="$2";  shift 2 ;;
    --mem)   ANX_MEM="$2";   shift 2 ;;
    --fb)    ANX_FB=1;        shift   ;;
    --help|-h)
      echo "Usage: $0 [--arch x86_64|arm64] [--mem 512M] [--fb]"
      echo "  --arch   Target architecture (default: host arch)"
      echo "  --mem    Guest RAM amount    (default: 512M)"
      echo "  --fb     Enable framebuffer display (default: serial console)"
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

# ── Locate QEMU ─────────────────────────────────────────────────────────────
LOCAL_QEMU="${REPO}/tools/qemu/bin/qemu-system-${ANX_ARCH}"
SYSTEM_QEMU="qemu-system-${ANX_ARCH}"

if [ -x "${LOCAL_QEMU}" ]; then
  QEMU="${LOCAL_QEMU}"
elif command -v "${SYSTEM_QEMU}" >/dev/null 2>&1; then
  QEMU="${SYSTEM_QEMU}"
else
  echo ""
  echo "Error: qemu-system-${ANX_ARCH} not found."
  echo ""
  echo "Install options:"
  case "$(uname -s)" in
    Darwin)
      echo "  Homebrew:  brew install qemu"
      echo "  From source: make qemu-deps" ;;
    Linux)
      echo "  Arch:   sudo pacman -S qemu-full"
      echo "  Debian: sudo apt install qemu-system-x86"
      echo "  Fedora: sudo dnf install qemu-system-x86" ;;
  esac
  echo ""
  exit 1
fi

QEMU_VERSION="$("${QEMU}" --version 2>/dev/null | head -1 || echo 'unknown')"
MIN_MAJOR=7
QEMU_MAJOR="$(echo "${QEMU_VERSION}" | grep -o '[0-9]*\.' | head -1 | tr -d '.')"
if [ -n "${QEMU_MAJOR}" ] && [ "${QEMU_MAJOR}" -lt "${MIN_MAJOR}" ] 2>/dev/null; then
  echo "Warning: QEMU ${QEMU_VERSION} detected; version ${MIN_MAJOR}.0+ recommended."
fi

# ── Detect acceleration ─────────────────────────────────────────────────────
ACCEL="tcg"
case "$(uname -s)" in
  Darwin)
    # Hypervisor.framework (HVF) — macOS, Apple Silicon and Intel
    if "${QEMU}" -accel help 2>/dev/null | grep -q hvf; then
      ACCEL="hvf"
    fi ;;
  Linux)
    if [ -r /dev/kvm ]; then
      ACCEL="kvm"
    fi ;;
esac

# ── Locate kernel image ──────────────────────────────────────────────────────
if [ "${ANX_ARCH}" = "arm64" ]; then
  KERNEL="${REPO}/build/anunix-arm64.elf"
else
  KERNEL="${REPO}/build/anunix-qemu.elf"
fi

if [ ! -f "${KERNEL}" ]; then
  echo ""
  echo "Kernel image not found: ${KERNEL}"
  echo "Building now..."
  echo ""
  if [ "${ANX_ARCH}" = "arm64" ]; then
    make -C "${REPO}" kernel ARCH=arm64
  else
    make -C "${REPO}" kernel
  fi
  echo ""
fi

# ── Build QEMU flags ─────────────────────────────────────────────────────────
if [ "${ANX_ARCH}" = "arm64" ]; then
  BASE_FLAGS="-M virt -cpu cortex-a72 -m ${ANX_MEM}"
  if [ "${ANX_FB}" = "1" ]; then
    DISPLAY_FLAGS="-device ramfb -serial mon:stdio"
  else
    DISPLAY_FLAGS="-nographic -serial mon:stdio"
  fi
  KERNEL_FLAGS="-kernel ${KERNEL}"
else
  BASE_FLAGS="-m ${ANX_MEM} -no-reboot"
  if [ "${ANX_FB}" = "1" ]; then
    DISPLAY_FLAGS="-serial mon:stdio -vga vmware"
  else
    DISPLAY_FLAGS="-nographic -serial mon:stdio"
  fi
  KERNEL_FLAGS="-kernel ${KERNEL}"
fi

NET_FLAGS="-netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=tcp::12222-:22 -device virtio-net-pci,netdev=net0"
ACCEL_FLAGS="-accel ${ACCEL}"

# ── Launch ───────────────────────────────────────────────────────────────────
echo ""
echo "Anunix ${VERSION}"
echo "──────────────────────────────────────────"
echo "  Arch:         ${ANX_ARCH}"
echo "  RAM:          ${ANX_MEM}"
echo "  Acceleration: ${ACCEL}"
echo "  Display:      $([ "${ANX_FB}" = "1" ] && echo "framebuffer" || echo "serial console")"
echo "  QEMU:         ${QEMU_VERSION}"
echo ""
echo "  HTTP API:     http://localhost:8080"
echo "  SSH:          ssh -p 12222 anunix@localhost  (password: anunix)"
echo ""
echo "  Ctrl+A X to exit QEMU"
echo "──────────────────────────────────────────"
echo ""

exec "${QEMU}" \
  ${BASE_FLAGS} \
  ${ACCEL_FLAGS} \
  ${DISPLAY_FLAGS} \
  ${NET_FLAGS} \
  ${KERNEL_FLAGS}
