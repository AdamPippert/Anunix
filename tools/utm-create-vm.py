#!/usr/bin/env python3
"""
utm-create-vm.py -- Create and register an Anunix ARM64 UTM VM.

UTM stores VMs as .utm bundles in:
  ~/Library/Containers/com.utmapp.UTM/Data/Documents/

This script:
  1. Builds the kernel (make kernel ARCH=arm64) if not already built
  2. Writes a UTM config.plist for a QEMU aarch64 virt VM
  3. Copies the .utm bundle into UTM's documents directory
  4. Prints instructions for opening UTM and starting the VM

Usage:
  python3 tools/utm-create-vm.py [--rebuild]
"""

import os
import sys
import uuid
import plistlib
import shutil
import subprocess
import argparse
from pathlib import Path

# --- Paths ---
REPO_ROOT   = Path(__file__).resolve().parent.parent
BUILD_DIR   = REPO_ROOT / "build" / "arm64"
KERNEL_ELF  = BUILD_DIR / "anunix.elf"
KERNEL_BIN  = BUILD_DIR / "anunix.bin"
UTM_DOCS    = Path.home() / "Library/Containers/com.utmapp.UTM/Data/Documents"
VM_NAME     = "Anunix ARM64"
UTM_BUNDLE  = UTM_DOCS / f"{VM_NAME}.utm"

def run(cmd, **kw):
    print(f"  $ {cmd}")
    result = subprocess.run(cmd, shell=True, **kw)
    if result.returncode != 0:
        print(f"  ERROR: command exited {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)
    return result

def build_kernel():
    print("Building kernel ARCH=arm64...")
    run(f"make -C {REPO_ROOT} kernel ARCH=arm64")
    if not KERNEL_ELF.exists():
        print(f"ERROR: expected {KERNEL_ELF} — build failed?", file=sys.stderr)
        sys.exit(1)
    print(f"  OK: {KERNEL_ELF} ({KERNEL_ELF.stat().st_size} bytes)")

def make_utm_config(kernel_path: str) -> dict:
    """
    Build the UTM config.plist dict for a bare-metal QEMU aarch64 VM.

    UTM 4.x QEMU VM plist structure (verified against UTM source):
      - Backend            : "QEMU"
      - ConfigurationVersion : 4
      - Name               : string
      - System             : dict with arch, machine, cpu, memory, boot
      - Displays           : [] (empty = nographic, no window)
      - Serials            : one BuiltinTerminal serial (UTM terminal window)
      - Sound              : []
      - Networks           : []
      - Drives             : []
      - Input              : minimal
      - QEMU               : additional args
    """
    vm_uuid = str(uuid.uuid4()).upper()

    # Additional QEMU args beyond what UTM sets from the structured config.
    # UTM handles -M virt, -cpu, -m, -kernel from the System.Boot dict.
    # We need: -nographic suppressed (UTM serial handles it), no extra needed.
    # UTM serial in BuiltinTerminal mode adds -serial to QEMU automatically.
    extra_args = []

    config = {
        "Backend": "QEMU",
        "ConfigurationVersion": 4,
        "Name": VM_NAME,
        "Information": {
            "UUID": vm_uuid,
            "Notes": (
                "Anunix ARM64 bare-metal kernel.\n"
                f"Kernel: {kernel_path}\n"
                "Machine: virt, CPU: max, RAM: 512 MB\n"
                "Serial: UTM built-in terminal (PL011 @ 0x09000000)\n"
                "Load address: 0x40080000\n"
            ),
        },
        "System": {
            "Architecture": "aarch64",
            "Boot": {
                # UTM passes this to QEMU as -kernel <ImagePath>
                "ImagePath": kernel_path,
            },
            "CPU": "max",
            "CPUCount": 4,
            "ForceMulticore": False,
            "Hypervisor": False,   # use TCG/QEMU not Apple Virtualization
            "MemorySize": 512,
            "Target": "virt",      # -M virt
        },
        "Input": {
            "UsbBusSupport": False,
        },
        # No display window -- we use the serial terminal
        "Displays": [],
        # One serial port mapped to UTM's built-in terminal window
        "Serials": [
            {
                "Mode": "BuiltinTerminal",
                "Target": "ManagementConsole",
                "Terminal": {
                    "Font":     "Menlo",
                    "FontSize": 12,
                    "Theme":    "Default",
                },
            }
        ],
        "Sound": [],
        "Networks": [],
        "Drives": [],
        # Any raw QEMU args UTM can't express structurally
        "QEMU": {
            "AdditionalArguments": extra_args,
            "DebugLog": False,
        },
        "Sharing": {
            "ClipboardSharing": False,
            "DirectoryShareMode": "None",
        },
    }
    return config

def create_utm_bundle(config: dict, force: bool = False):
    if UTM_BUNDLE.exists():
        if not force:
            print(f"\nVM bundle already exists: {UTM_BUNDLE}")
            print("Use --force to overwrite.")
            return False
        print(f"Removing existing bundle: {UTM_BUNDLE}")
        shutil.rmtree(UTM_BUNDLE)

    print(f"\nCreating UTM bundle: {UTM_BUNDLE}")
    UTM_BUNDLE.mkdir(parents=True)

    config_path = UTM_BUNDLE / "config.plist"
    with open(config_path, "wb") as f:
        plistlib.dump(config, f, fmt=plistlib.FMT_XML, sort_keys=False)

    print(f"  Wrote config.plist ({config_path.stat().st_size} bytes)")
    return True

def main():
    parser = argparse.ArgumentParser(description="Create Anunix ARM64 UTM VM")
    parser.add_argument("--rebuild", action="store_true", help="Force kernel rebuild")
    parser.add_argument("--force",   action="store_true", help="Overwrite existing VM bundle")
    parser.add_argument("--kernel",  default=None,        help="Override kernel ELF path")
    args = parser.parse_args()

    # Determine kernel path
    if args.kernel:
        kernel = Path(args.kernel).resolve()
    else:
        kernel = KERNEL_ELF

    # Build if needed
    if args.rebuild or not kernel.exists():
        build_kernel()

    if not kernel.exists():
        print(f"ERROR: kernel not found at {kernel}", file=sys.stderr)
        print("Run: make kernel ARCH=arm64", file=sys.stderr)
        sys.exit(1)

    print(f"\nKernel: {kernel} ({kernel.stat().st_size:,} bytes)")

    # Check UTM documents dir
    if not UTM_DOCS.exists():
        print(f"\nERROR: UTM documents directory not found: {UTM_DOCS}")
        print("Is UTM installed? Install from https://mac.getutm.app")
        sys.exit(1)

    # Create config and bundle
    config = make_utm_config(str(kernel))
    created = create_utm_bundle(config, force=args.force)

    if created:
        print("\n--- VM created successfully ---")
    else:
        print("\n--- VM bundle already exists ---")

    print(f"""
Next steps:
  1. Open UTM (or bring it to the foreground)
  2. File > Open... > select {UTM_BUNDLE}
     OR: UTM will auto-detect it on next launch from the Documents folder
  3. Click "Anunix ARM64" -> Start
  4. The UTM terminal window opens -- you should see kernel boot output

  To rebuild and reload:
    make kernel ARCH=arm64 && python3 tools/utm-create-vm.py --force

  For headless QEMU (faster iteration):
    make qemu ARCH=arm64
""")

if __name__ == "__main__":
    main()
