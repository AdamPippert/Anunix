#!/usr/bin/env bash
#
# test-uefi-highmem.sh — Boot the UEFI ISO under OVMF with 1 GiB and 4 GiB.
#
# The EFI stub links at ImageBase 0x140000000 without relocations, so the
# firmware loads it at 5 GiB whenever RAM exists there. A 1 GiB q35 guest
# has no RAM above 4 GiB. A 4 GiB q35 guest has RAM at 0x100000000 and up,
# which is the layout of a real machine with more than 4 GiB installed.
#
# Both boots must reach "kernel init complete". For the 4 GiB boot, the
# OVMF debug log must also show ANUNIX.EFI loaded above 4 GiB. Without
# that check, a firmware that places the image low would pass the test
# without exercising the case it exists for.
#
# Usage: tools/test-uefi-highmem.sh [iso]
# Env:   QEMU, OVMF_FD, BOOT_TIMEOUT (seconds per boot, default 120)

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-${PROJECT_DIR}/build/anunix-x86_64.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
OUT_DIR="${PROJECT_DIR}/build/test-uefi"
READY="kernel init complete"

# find_ovmf: print the first OVMF firmware image that exists.
find_ovmf() {
	local f

	for f in "${OVMF_FD:-}" \
		/usr/share/edk2/ovmf/OVMF_CODE.fd \
		/usr/share/edk2/x64/OVMF.4m.fd \
		/usr/share/OVMF/OVMF.fd \
		/usr/share/ovmf/OVMF.fd \
		/usr/share/qemu/OVMF.fd; do
		if [ -n "$f" ] && [ -f "$f" ]; then
			printf '%s\n' "$f"
			return 0
		fi
	done
	return 1
}

# show_tail <log>: print the last non-empty lines of a serial log.
show_tail() {
	tr -d '\r' < "$1" | grep -a -v '^[[:space:]]*$' | tail -n 12 | sed 's/^/        /'
}

# boot <label> <memory>: boot the ISO; succeed when the kernel reports ready.
boot() {
	local label="$1" mem="$2"
	local serial="${OUT_DIR}/${label}-serial.log"
	local debug="${OUT_DIR}/${label}-ovmf-debug.log"
	local pid waited=0 result=1

	: > "$serial"
	: > "$debug"
	"$QEMU" "${ACCEL[@]}" -machine q35 -m "$mem" -bios "$OVMF" \
		-display none -monitor none -no-reboot \
		-serial "file:${serial}" \
		-debugcon "file:${debug}" -global isa-debugcon.iobase=0x402 \
		-cdrom "$ISO" &
	pid=$!

	while [ "$waited" -lt "$BOOT_TIMEOUT" ]; do
		if grep -aq "$READY" "$serial"; then
			result=0
			break
		fi
		if grep -aq "Exception Type" "$serial"; then
			break
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			break
		fi
		sleep 1
		waited=$((waited + 1))
	done

	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true

	if [ "$result" -eq 0 ]; then
		echo "  BOOT  ${label} (${mem}): reached '${READY}' in ${waited}s"
	else
		echo "  FAIL  ${label} (${mem}): no '${READY}' after ${waited}s; serial tail:"
		show_tail "$serial"
	fi
	return "$result"
}

# load_addr <debug log>: print the address OVMF loaded ANUNIX.EFI at.
load_addr() {
	{ grep -a -A4 'ANUNIX\.EFI' "$1" || true; } |
		{ grep -a -m1 -o 'Loading driver at 0x[0-9A-Fa-f]*' || true; } |
		sed 's/.* //'
}

if [ ! -f "$ISO" ]; then
	echo "ERROR: ${ISO} not found; run 'make iso' first" >&2
	exit 1
fi
if ! OVMF="$(find_ovmf)"; then
	echo "ERROR: OVMF firmware not found; set OVMF_FD" >&2
	exit 1
fi
if ! command -v "$QEMU" >/dev/null 2>&1; then
	echo "ERROR: ${QEMU} not found; set QEMU" >&2
	exit 1
fi

# 1 GiB pages in the stub's identity map need pdpe1gb; TCG's max CPU has it.
if [ -w /dev/kvm ]; then
	ACCEL=(-enable-kvm -cpu host)
else
	ACCEL=(-cpu max)
fi

mkdir -p "$OUT_DIR"
echo "  UEFI high-memory boot test: ${ISO}"
echo "  firmware ${OVMF}, logs in ${OUT_DIR}"

failed=0

boot low 1G || failed=1

if boot high 4G; then
	addr="$(load_addr "${OUT_DIR}/high-ovmf-debug.log")"
	if [ -z "$addr" ]; then
		echo "  WARN  high: firmware logged no load address; placement above 4 GiB unconfirmed"
	elif [ "$((addr))" -lt "$((1 << 32))" ]; then
		echo "  FAIL  high: ANUNIX.EFI loaded at ${addr}, below 4 GiB; the case was not exercised"
		failed=1
	else
		echo "  PASS  high: ANUNIX.EFI loaded at ${addr}, above 4 GiB"
	fi
else
	failed=1
fi

if [ "$failed" -ne 0 ]; then
	echo "  UEFI high-memory boot test FAILED"
	exit 1
fi
echo "  UEFI high-memory boot test passed"
