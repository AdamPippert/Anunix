#!/usr/bin/env python3
"""Boot one research image and save native regression evidence."""

import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import time
import urllib.request
from kernel_profile import create_profile, digest, load_profile, validate_guest, validate_profile
from source_identity import artifact_source, validate_source_guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    boot = parser.add_mutually_exclusive_group(required=True)
    boot.add_argument("--kernel", type=Path)
    boot.add_argument("--image", type=Path)
    parser.add_argument("--firmware", type=Path, default=Path("/usr/share/OVMF/OVMF_CODE.fd"))
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--test", action="append", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--profile", type=Path, help="existing artifact profile; otherwise capture one before boot")
    parser.add_argument("--expect-failure", action="store_true")
    args = parser.parse_args()
    if any(not re.fullmatch(r"day-[0-9]{3}", name) for name in args.test):
        parser.error("test names must use day-NNN")
    artifact = (args.kernel or args.image).resolve(strict=True)
    try:
        profile = load_profile(args.profile) if args.profile else create_profile(artifact, args.revision)
        validate_profile(profile, artifact, args.revision, "x86_64", 1)
        source_sha256 = artifact_source(artifact)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "kernel-profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-m", "1G",
               "-display", "none", "-no-reboot", "-monitor", "none",
               "-serial", f"file:{out / 'serial.log'}",
               "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:8080",
               "-device", "virtio-net-pci,netdev=n0"]
    if args.kernel:
        command += ["-kernel", str(artifact)]
    else:
        command += ["-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware.resolve(strict=True)}",
                    "-drive", f"if=ide,format=raw,snapshot=on,file={artifact}"]
    report = {"revision": args.revision, "artifact": str(artifact),
              "boot_mode": "kernel" if args.kernel else "uefi",
              "source_sha256": source_sha256, "source_verified": False,
              "sha256": digest(artifact), "qemu_command": command,
              "qemu_version": subprocess.check_output([command[0], "--version"], text=True).splitlines()[0],
              "kernel_profile": profile, "profile_verified": False,
              "tests": [], "passed": False, "expected_failure": args.expect_failure}
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, payload=None):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                                     headers={"Content-Type": "application/json"})
        with opener.open(req, timeout=3) as response:
            return json.load(response)

    with (out / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 45
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited with status {process.returncode}")
                try:
                    report["health"] = request("/api/v1/health")
                    break
                except (OSError, ValueError):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("guest health timeout")
                    time.sleep(0.5)
            report["sysinfo"] = request("/api/v1/exec", {"command": "sysinfo"})
            validate_guest(profile, report["sysinfo"].get("output", ""))
            validate_source_guest(source_sha256, report["sysinfo"].get("output", ""))
            for name in args.test:
                response = request("/api/v1/exec", {"command": f"research-test {name}"})
                marker = f"RESEARCH {name} PASS rc=0"
                passed = marker in response.get("output", "")
                failed = f"RESEARCH {name} FAIL rc=" in response.get("output", "")
                report["tests"].append({"name": name, "passed": passed, "response": response})
                if args.expect_failure:
                    if not failed or passed:
                        raise RuntimeError(f"{name}: expected an explicit regression failure")
                elif not passed or failed:
                    raise RuntimeError(f"{name}: native regression failed")
            if process.poll() is not None:
                raise RuntimeError("guest exited during tests")
            validate_profile(profile, artifact, args.revision, "x86_64", 1)
            if artifact_source(artifact) != source_sha256:
                raise ValueError("source fingerprint changed during validation")
            report["source_verified"] = True
            report["profile_verified"] = True
            report["passed"] = not args.expect_failure
            report["expected_result_observed"] = True
        except Exception as error:
            report["error"] = str(error)
            report["expected_result_observed"] = False
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"evidence": str(out / "result.json"), "passed": report["passed"],
                      "expected_result_observed": report["expected_result_observed"],
                      "error": report.get("error")}))
    return 0 if report["expected_result_observed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
