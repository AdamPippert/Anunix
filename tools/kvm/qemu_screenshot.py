#!/usr/bin/env python3
"""
qemu_screenshot.py — Boot Anunix in QEMU and capture a screenshot or boot video.

Usage:
  python3 tools/kvm/qemu_screenshot.py [output.jpg]
  python3 tools/kvm/qemu_screenshot.py --video [output.mp4]

Single-frame mode: waits for boot, captures one JPEG.
Video mode: captures frames at ~20 fps from first boot output until the
display has not changed for 10 seconds, then assembles an MP4 (requires
ffmpeg) or leaves a directory of numbered JPEGs if ffmpeg is absent.
"""

import os
import sys
import time
import socket
import subprocess
import shutil
import hashlib
import urllib.request

KERNEL_ELF   = "build/x86_64/anunix-qemu.elf"
MON_SOCK     = "/tmp/anunix-qemu-mon.sock"
SERIAL_LOG   = "/tmp/anunix-serial.log"
PPM_FILE     = "/tmp/anunix-screen.ppm"
HTTP_PORT    = 19080
BOOT_MARKER  = "kernel init complete"
BOOT_TIMEOUT = 45

VIDEO_FPS         = 20
VIDEO_FRAME_MS    = 1000 // VIDEO_FPS   # 50 ms
VIDEO_IDLE_SECS   = 10   # stop after this many seconds of no pixel change
VIDEO_MAX_SECS    = 120  # hard cap

def find_qemu():
    local = "tools/qemu/bin/qemu-system-x86_64"
    if os.path.exists(local):
        return os.path.abspath(local)
    q = shutil.which("qemu-system-x86_64")
    if q:
        return q
    raise RuntimeError("qemu-system-x86_64 not found; run 'make qemu-deps'")

def netdev_args():
    qemu = find_qemu()
    result = subprocess.run([qemu, "-netdev", "help"],
                            capture_output=True, text=True)
    backends = result.stdout + result.stderr
    # Match "user" as a whole word — avoid false-positive from "vhost-user"
    import re
    if re.search(r'\buser\b', backends):
        return ["-netdev", f"user,id=net0,hostfwd=tcp::{HTTP_PORT}-:8080",
                "-device", "virtio-net-pci,netdev=net0"]
    if "vmnet-shared" in backends:
        return ["-netdev", "vmnet-shared,id=net0",
                "-device", "virtio-net-pci,netdev=net0"]
    return []

def wait_for_serial(log_path, marker, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(log_path):
            with open(log_path, "r", errors="replace") as f:
                if marker in f.read():
                    return True
        time.sleep(0.5)
    return False

def monitor_screendump(sock_path, ppm_path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(5)
    buf = b""
    try:
        while b"(qemu)" not in buf:
            buf += s.recv(4096)
    except socket.timeout:
        pass
    s.settimeout(None)
    s.send(f"screendump {ppm_path}\n".encode())
    time.sleep(0.06)  # give QEMU time to write
    s.close()

def ppm_hash(ppm_path):
    """MD5 of first 8KB — fast enough to detect display changes."""
    try:
        with open(ppm_path, "rb") as f:
            return hashlib.md5(f.read(8192)).hexdigest()
    except OSError:
        return ""

def ppm_to_jpeg(ppm_path, jpg_path):
    if shutil.which("sips"):
        subprocess.run(
            ["sips", "-s", "format", "jpeg", ppm_path, "--out", jpg_path],
            check=True, capture_output=True)
    elif shutil.which("convert"):
        subprocess.run(["convert", ppm_path, jpg_path],
                       check=True, capture_output=True)
    else:
        shutil.copy(ppm_path, jpg_path)

def boot_qemu():
    for f in (MON_SOCK, PPM_FILE, SERIAL_LOG):
        if os.path.exists(f):
            os.remove(f)

    qemu = find_qemu()
    net  = netdev_args()
    if not net:
        print("  (no network backend — using serial-only boot detection)")

    cmd = ([qemu, "-m", "512M", "-no-reboot",
            "-vga", "std", "-display", "none",
            "-monitor", f"unix:{MON_SOCK},server,nowait",
            "-serial", f"file:{SERIAL_LOG}",
            "-kernel", KERNEL_ELF]
           + net)

    print(f"booting {KERNEL_ELF} ...")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print(f"waiting for '{BOOT_MARKER}' ...")
    if not wait_for_serial(SERIAL_LOG, BOOT_MARKER, BOOT_TIMEOUT):
        print("ERROR: boot marker not seen in time")
        if os.path.exists(SERIAL_LOG):
            with open(SERIAL_LOG) as f:
                print(f.read()[-2000:])
        proc.terminate()
        sys.exit(1)

    return proc

# ------------------------------------------------------------------ #
# Single-frame mode                                                    #
# ------------------------------------------------------------------ #

def single_frame(out_path):
    proc = boot_qemu()
    try:
        print("boot complete — waiting 3s for WM render ...")
        time.sleep(3)

        print(f"screendump -> {PPM_FILE}")
        monitor_screendump(MON_SOCK, PPM_FILE)

        if not os.path.exists(PPM_FILE) or os.path.getsize(PPM_FILE) < 1000:
            print("ERROR: screendump empty (framebuffer may not be active)")
            proc.terminate()
            sys.exit(1)

        ppm_to_jpeg(PPM_FILE, out_path)
        print(f"screenshot saved: {out_path}  ({os.path.getsize(out_path):,} bytes)")
    finally:
        proc.terminate()
        proc.wait()

# ------------------------------------------------------------------ #
# Video mode                                                           #
# ------------------------------------------------------------------ #

def record_video(out_path):
    frames_dir = "/tmp/anunix-frames"
    shutil.rmtree(frames_dir, ignore_errors=True)
    os.makedirs(frames_dir)

    proc = boot_qemu()
    try:
        print("recording frames at 20 fps (idle timeout: 10s, hard cap: 120s) ...")
        frame_idx   = 0
        last_hash   = ""
        last_change = time.time()
        deadline    = time.time() + VIDEO_MAX_SECS
        interval    = VIDEO_FRAME_MS / 1000.0

        while time.time() < deadline:
            t0 = time.time()

            tmp_ppm = f"/tmp/anunix-frame-{frame_idx}.ppm"
            monitor_screendump(MON_SOCK, tmp_ppm)

            h = ppm_hash(tmp_ppm)
            if h and h != last_hash:
                last_hash   = h
                last_change = time.time()

            if h:
                jpg = os.path.join(frames_dir, f"frame_{frame_idx:06d}.jpg")
                ppm_to_jpeg(tmp_ppm, jpg)
                frame_idx += 1
                if os.path.exists(tmp_ppm):
                    os.remove(tmp_ppm)

            idle = time.time() - last_change
            if idle >= VIDEO_IDLE_SECS and frame_idx > 0:
                print(f"  display idle for {idle:.1f}s — stopping at frame {frame_idx}")
                break

            elapsed = time.time() - t0
            sleep_for = max(0.0, interval - elapsed)
            if sleep_for > 0:
                time.sleep(sleep_for)

        print(f"captured {frame_idx} frames")

        if frame_idx == 0:
            print("ERROR: no frames captured")
            sys.exit(1)

        # Assemble video
        if shutil.which("ffmpeg"):
            subprocess.run([
                "ffmpeg", "-y",
                "-framerate", str(VIDEO_FPS),
                "-i", os.path.join(frames_dir, "frame_%06d.jpg"),
                "-c:v", "libx264", "-pix_fmt", "yuv420p",
                "-crf", "18",
                out_path
            ], check=True)
            print(f"video saved: {out_path}  ({os.path.getsize(out_path):,} bytes)")
        else:
            print(f"ffmpeg not found — frames saved in {frames_dir}/")
            print(f"  assemble with: ffmpeg -framerate {VIDEO_FPS} -i {frames_dir}/frame_%06d.jpg -c:v libx264 {out_path}")

    finally:
        proc.terminate()
        proc.wait()

# ------------------------------------------------------------------ #
# Entry point                                                          #
# ------------------------------------------------------------------ #

def main():
    args = sys.argv[1:]
    video = "--video" in args
    args  = [a for a in args if a != "--video"]

    if video:
        out = args[0] if args else "/tmp/anunix-boot.mp4"
        record_video(out)
    else:
        out = args[0] if args else "/tmp/anunix-screenshot.jpg"
        single_frame(out)

if __name__ == "__main__":
    main()
