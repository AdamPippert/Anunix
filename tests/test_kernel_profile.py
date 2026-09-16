"""Artifact and pre-boot failures for the kernel-profile laboratory."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from kernel_profile import create_profile, load_profile, validate_guest, validate_profile


class KernelProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifact = self.root / "kernel.bin"
        self.marker = b"ANUNIX_KERNEL_PROFILE_V1 arch=x86_64 research_test=1\n"
        self.artifact.write_bytes(b"kernel\x00" + self.marker + b"payload")
        self.profile = create_profile(self.artifact, "abcdef0")

    def check(self, profile=None, architecture="x86_64", research_test=1):
        validate_profile(self.profile if profile is None else profile,
                         self.artifact, "abcdef0", architecture, research_test)

    def test_valid_round_trip_and_guest(self):
        path = self.root / "profile.json"
        path.write_text(json.dumps(self.profile))
        self.check(load_profile(path))
        validate_guest(self.profile, "System information\n" + self.marker.decode())

    def test_modified_image_with_same_size(self):
        self.artifact.write_bytes(self.artifact.read_bytes()[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "digest or size mismatch"):
            self.check()

    def test_wrong_configuration_is_rejected_before_qemu(self):
        self.artifact.write_bytes(self.marker.replace(b"x86_64", b"arm64"))
        path = self.root / "profile.json"
        path.write_text(json.dumps(create_profile(self.artifact, "abcdef0")))
        output = self.root / "vm"
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/research_vm.py"), "--kernel", str(self.artifact),
             "--profile", str(path), "--revision", "abcdef0", "--out-dir", str(output),
             "--test", "day-018"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("incompatible required configuration", result.stderr)
        self.assertFalse(output.exists())

    def test_production_image_rejected_by_research_requirement(self):
        self.artifact.write_bytes(self.marker.replace(b"research_test=1", b"research_test=0"))
        profile = create_profile(self.artifact, "abcdef0")
        with self.assertRaisesRegex(ValueError, "incompatible required configuration"):
            self.check(profile)
        self.check(profile, research_test=0)

    def test_profile_cannot_relabel_compiled_image(self):
        profile = copy.deepcopy(self.profile)
        profile["configuration"]["architecture"] = "arm64"
        with self.assertRaisesRegex(ValueError, "compiled configuration"):
            self.check(profile, architecture="arm64")

    def test_missing_conflicting_or_future_markers(self):
        for contents in (b"unmarked kernel", self.marker + self.marker.replace(b"=1", b"=0"),
                         self.marker.replace(b"V1", b"V2"), b""):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                self.artifact.write_bytes(contents)
                create_profile(self.artifact, "abcdef0")
        # The disk image can contain a kernel and its identical legacy wrapper.
        self.artifact.write_bytes(self.marker + self.marker)
        profile = create_profile(self.artifact, "abcdef0")
        self.check(profile)

    def test_guest_must_report_exactly_one_matching_marker(self):
        for output in ("", self.marker.decode() * 2, self.marker.decode().replace("x86_64", "arm64"),
                       self.marker.decode().replace("=1", "=0")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                validate_guest(self.profile, output)

    def test_strict_profile_schema(self):
        invalid = [None, [], {}, {**self.profile, "promotion_approved": True}]
        for key, value in (("schema", True), ("schema", 2), ("declared_revision", "feed123"),
                           ("artifact_bytes", True), ("artifact_bytes", 0),
                           ("artifact_bytes", self.profile["artifact_bytes"] + 1),
                           ("artifact_sha256", "0" * 64), ("artifact_sha256", "not-a-hash"),
                           ("configuration", {"architecture": "x86_64", "research_test": True}),
                           ("configuration", {"architecture": "unknown", "research_test": 1})):
            invalid.append({**self.profile, key: value})
        for profile in invalid:
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                validate_profile(profile, self.artifact, "abcdef0", "x86_64", 1)

    def test_duplicate_json_keys(self):
        path = self.root / "profile.json"
        path.write_text('{"schema":1,"schema":2}')
        with self.assertRaisesRegex(ValueError, "duplicate"):
            load_profile(path)


if __name__ == "__main__":
    unittest.main()
