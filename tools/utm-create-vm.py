#!/usr/bin/env python3
"""
utm-create-vm.py -- Create and register an Anunix UTM VM.

Supports two targets:
  --arch x86_64   QEMU x86_64 emulation  (works on any Mac, default)
  --arch arm64    QEMU aarch64 TCG        (requires arm64 kernel build)

UTM stores VMs as .utm bundles in:
  ~/Library/Containers/com.utmapp.UTM/Data/Documents/

The script:
  1. Builds the kernel for the target arch if needed
  2. Copies the kernel ELF into the .utm bundle (so UTM can find it)
  3. Writes a verified config.plist (structure validated against UTM 4.x)
  4. Prints open/start instructions

Usage:
  python3 tools/utm-create-vm.py [--arch x86_64|arm64] [--rebuild] [--force]
"""

import os
import sys
import uuid
import plistlib
import shutil
import subprocess
import argparse
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
UTM_DOCS  = Path.home() / "Library/Containers/com.utmapp.UTM/Data/Documents"


def run(cmd: str, cwd=None):
    print(f"  $ {cmd}")
    r = subprocess.run(cmd, shell=True, cwd=cwd)
    if r.returncode != 0:
        print(f"  ERROR: exited {r.returncode}", file=sys.stderr)
        sys.exit(r.returncode)


# ---------------------------------------------------------------------------
# UTM config.plist builder
# ---------------------------------------------------------------------------
# Key names verified by reading an existing UTM 4.x QEMU VM plist.
# (Plural names like "Displays" or "Drives" are WRONG — UTM uses singular.)

def make_config(arch: str, kernel_bundle_path: str, vm_name: str) -> dict:
    """
    arch: "x86_64" or "aarch64"
    kernel_bundle_path: path to the kernel ELF *inside* the .utm bundle
                        (relative is fine; UTM resolves against the bundle)
    """
    vm_uuid = str(uuid.uuid4()).upper()

    if arch == "x86_64":
        utm_arch   = "x86_64"
        utm_target = "pc"           # Standard PC (PIIX4) — broadest compat
        utm_cpu    = "qemu64"
        machine_desc = "QEMU x86_64 emulation — works on any Mac (slower)"
        # -kernel loads a multiboot/ELF kernel directly; serial on COM1
        extra_args = [
            "-kernel", kernel_bundle_path,
            "-serial", "mon:stdio",
            "-append", "console=ttyS0",
            "-m", "512M",
            "-no-reboot",
        ]
    else:
        utm_arch   = "aarch64"
        utm_target = "virt"
        utm_cpu    = "cortex-a72"
        machine_desc = "QEMU aarch64 TCG — ARM64 kernel required"
        extra_args = [
            "-kernel", kernel_bundle_path,
            "-serial", "mon:stdio",
            "-append", "console=ttyAMA0",
            "-m", "512M",
            "-no-reboot",
        ]

    # UTM 4.x wants AdditionalArguments as a list of dicts: [{"string": "-flag"}, ...]
    additional_args = [{"string": a} for a in extra_args]

    return {
        "Backend": "QEMU",
        "ConfigurationVersion": 4,
        "Information": {
            "Icon": "cpu",
            "IconCustom": False,
            "Name": vm_name,
            "UUID": vm_uuid,
        },
        "System": {
            "Architecture": utm_arch,
            "CPU": utm_cpu,
            "CPUCount": 2,
            "CPUFlagsAdd": [],
            "CPUFlagsRemove": [],
            "ForceMulticore": False,
            "JITCacheSize": 0,
            "MemorySize": 512,
            "Target": utm_target,
        },
        "QEMU": {
            "AdditionalArguments": additional_args,
            "BalloonDevice": False,
            "DebugLog": False,
            "Hypervisor": False,    # TCG emulation, not Apple Virt.framework
            "PS2Controller": arch == "x86_64",
            "RNGDevice": False,
            "RTCLocalTime": False,
            "TPMDevice": False,
            "TSO": False,
            "UEFIBoot": False,      # direct -kernel load, no UEFI firmware
        },
        # virtio-gpu-pci — the only display hardware UTM 4.x validates on QEMU
        # QEMU also exposes a legacy VGA at 0xA0000 for multiboot2 framebuffer
        "Display": [
            {
                "DownscalingFilter": "Linear",
                "DynamicResolution": False,
                "Hardware": "virtio-gpu-pci",
                "NativeResolution": False,
                "UpscalingFilter": "Nearest",
            }
        ],
        # Serial left empty — serial output goes to QEMU stdio/debug log
        "Serial": [],
        "Drive": [],
        "Network": [],
        "Sound": [],
        "Input": {
            "MaximumUsbShare": 3,
            "UsbBusSupport": "3.0",
            "UsbSharing": False,
        },
        "Sharing": {
            "ClipboardSharing": False,
            "DirectoryShareMode": "None",
            "DirectoryShareReadOnly": False,
        },
    }


# ---------------------------------------------------------------------------
# Bundle creation
# ---------------------------------------------------------------------------

def create_bundle(arch: str, kernel_src: Path, force: bool) -> Path:
    vm_name = f"Anunix {arch.upper()}"
    bundle  = UTM_DOCS / f"{vm_name}.utm"

    if bundle.exists():
        if not force:
            print(f"\nBundle already exists: {bundle}")
            print("Use --force to overwrite it.")
            sys.exit(0)
        print(f"Removing existing bundle: {bundle}")
        shutil.rmtree(bundle)

    print(f"\nCreating: {bundle}")
    bundle.mkdir(parents=True)

    # Copy kernel into bundle so UTM's sandbox can reach it
    kernel_dest = bundle / "anunix.elf"
    shutil.copy2(kernel_src, kernel_dest)
    print(f"  kernel  → {kernel_dest.name}  "
          f"({kernel_dest.stat().st_size:,} bytes)")

    # The -kernel arg references the file *inside* the bundle.
    # UTM resolves relative paths against the bundle directory,
    # but absolute paths also work since we own the bundle.
    kernel_in_bundle = str(kernel_dest)

    config = make_config(
        arch="x86_64" if arch == "x86_64" else "aarch64",
        kernel_bundle_path=kernel_in_bundle,
        vm_name=vm_name,
    )

    config_path = bundle / "config.plist"
    with open(config_path, "wb") as f:
        plistlib.dump(config, f, fmt=plistlib.FMT_XML, sort_keys=False)
    print(f"  config  → config.plist  ({config_path.stat().st_size} bytes)")

    return bundle


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Create an Anunix UTM VM for x86_64 or arm64")
    parser.add_argument("--arch",    default="x86_64",
                        choices=["x86_64", "arm64"],
                        help="Target architecture (default: x86_64)")
    parser.add_argument("--rebuild", action="store_true",
                        help="Force kernel rebuild before creating VM")
    parser.add_argument("--force",   action="store_true",
                        help="Overwrite existing VM bundle")
    parser.add_argument("--kernel",  default=None,
                        help="Override kernel ELF path")
    args = parser.parse_args()

    # Resolve kernel path
    if args.kernel:
        kernel = Path(args.kernel).resolve()
    else:
        kernel = REPO_ROOT / "build" / args.arch / "anunix.elf"

    # Build if needed
    if args.rebuild or not kernel.exists():
        print(f"Building kernel ARCH={args.arch}...")
        run(f"make kernel ARCH={args.arch}", cwd=REPO_ROOT)

    if not kernel.exists():
        print(f"ERROR: kernel not found: {kernel}", file=sys.stderr)
        print(f"  Run: make kernel ARCH={args.arch}", file=sys.stderr)
        sys.exit(1)

    print(f"Kernel : {kernel}  ({kernel.stat().st_size:,} bytes)")

    # Check UTM is installed
    if not UTM_DOCS.exists():
        print(f"\nERROR: UTM documents directory not found:\n  {UTM_DOCS}")
        print("Install UTM from https://mac.getutm.app")
        sys.exit(1)

    bundle = create_bundle(args.arch, kernel, force=args.force)

    print(f"""
=== VM created: {bundle.name} ===

To start the VM:
  1. Open UTM  (or bring it to the foreground if already open)
  2. If the VM doesn't appear in the sidebar automatically:
       File ▸ Open...  →  select  {bundle}
  3. Click  "{bundle.stem}"  →  ▶ Start
  4. UTM opens a terminal window — Anunix kernel output appears there

To rebuild and reload after code changes:
  make kernel ARCH={args.arch} && \\
  python3 tools/utm-create-vm.py --arch {args.arch} --force
""")


if __name__ == "__main__":
    main()
