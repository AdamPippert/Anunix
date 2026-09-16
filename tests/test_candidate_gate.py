"""Candidate evidence rejects changed sources, artifacts, and incomplete VM results."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from candidate_gate import validate_candidate
from kernel_profile import create_profile
from source_identity import PREFIX, artifact_source, source_fingerprint


class CandidateGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "kernel").mkdir()
        (self.root / "tools").mkdir()
        (self.root / "Makefile").write_text("kernel: main.c\n")
        (self.root / "kernel/main.c").write_text("int policy = 1;\n")
        (self.root / "tools/source_identity.py").write_text("# generator fixture\n")
        self.source = source_fingerprint(self.root)
        self.marker = "ANUNIX_KERNEL_PROFILE_V1 arch=x86_64 research_test=1\n" + PREFIX + self.source + "\n"
        self.kernel = self.root / "kernel.bin"
        self.image = self.root / "image.bin"
        self.kernel.write_bytes(self.marker.encode() + b"kernel")
        self.image.write_bytes(self.marker.encode() * 2 + b"image")
        self.kernel_report = self.root / "kernel.json"
        self.image_report = self.root / "image.json"
        self.host = self.root / "host.log"
        self.conformance = self.root / "conformance.log"
        self.host_text = ("59 passed, 0 failed\nRan 9 tests in 0.01s\n\nOK\nRan 12 tests in 0.01s\n\nOK\n"
                          + "ANUNIX_HOST_SOURCE_V1 sha256=" + self.source + "\n")
        self.host.write_text(self.host_text)
        self.conformance.write_text("threshold gate: PASS\nANUNIX_CONFORMANCE_SOURCE_V1 sha256=" + self.source + "\n")
        for mode, artifact, path in (("kernel", self.kernel, self.kernel_report), ("uefi", self.image, self.image_report)):
            profile = create_profile(artifact, "abcdef0")
            report = {"revision": "abcdef0", "boot_mode": mode, "sha256": profile["artifact_sha256"],
                      "kernel_profile": profile, "source_sha256": self.source, "profile_verified": True,
                      "source_verified": True, "passed": True, "expected_result_observed": True,
                      "expected_failure": False, "sysinfo": {"status": "ok", "output": self.marker},
                      "tests": [{"name": name, "passed": True,
                                 "response": {"status": "ok", "output": f"RESEARCH {name} PASS rc=0\n"}}
                                for name in ("day-001", "day-002", "day-002")]}
            path.write_text(json.dumps(report))

    def check(self, day=2):
        return validate_candidate(self.root, "abcdef0", day, self.kernel, self.image,
                                  self.kernel_report, self.image_report, self.host, self.conformance)

    def test_valid_candidate_and_duplicate_identical_source_markers(self):
        result = self.check()
        self.assertTrue(result["validated"])
        self.assertEqual(result["source_sha256"], self.source)
        self.assertEqual(len(result["evidence_sha256"]), 4)
        self.assertEqual(artifact_source(self.image), self.source)

    def test_absent_validation_never_writes_record(self):
        self.image_report.unlink()
        output = self.root / "accepted.json"
        args = [sys.executable, str(ROOT / "tools/candidate_gate.py"), "--revision", "abcdef0", "--through-day", "2"]
        for name, value in (("source", self.root), ("kernel", self.kernel), ("image", self.image),
                            ("kernel-report", self.kernel_report), ("image-report", self.image_report),
                            ("host-log", self.host), ("conformance-log", self.conformance), ("output", output)):
            args += ["--" + name, str(value)]
        process = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)
        self.assertFalse(output.exists())

    def test_source_change_rejects_existing_reports(self):
        (self.root / "kernel/main.c").write_text("int policy = 2;\n")
        with self.assertRaisesRegex(ValueError, "source candidate"):
            self.check()

    def test_same_size_image_change_rejects_existing_reports(self):
        self.image.write_bytes(self.image.read_bytes()[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "digest or size"):
            self.check()

    def test_report_cannot_relabel_mode_revision_or_artifact(self):
        original = json.loads(self.image_report.read_text())
        for key, value in (("revision", "fedcba0"), ("boot_mode", "kernel"), ("sha256", "a" * 64),
                           ("source_sha256", "b" * 64), ("passed", 1), ("expected_failure", True),
                           ("expected_result_observed", False), ("profile_verified", False),
                           ("source_verified", False), ("error", "failure")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.image_report.write_text(json.dumps({**original, key: value}))
                self.check()

    def test_incomplete_duplicate_or_false_regressions_rejected(self):
        original = json.loads(self.image_report.read_text())
        mutations = []
        for tests in ([], original["tests"][:-1], list(reversed(original["tests"])), original["tests"] * 2):
            mutations.append({**original, "tests": tests})
        for output in ("", "RESEARCH day-001 FAIL rc=-1\n", "prefix RESEARCH day-001 PASS rc=0\n",
                       "RESEARCH day-001 PASS rc=0\nRESEARCH day-001 FAIL rc=-1\n"):
            report = copy.deepcopy(original)
            report["tests"][0]["response"]["output"] = output
            mutations.append(report)
        for report in mutations:
            with self.subTest(report=report["tests"]), self.assertRaises(ValueError):
                self.image_report.write_text(json.dumps(report))
                self.check()

    def test_wrong_or_repeated_guest_source_rejected(self):
        original = json.loads(self.image_report.read_text())
        for output in (self.marker.replace(self.source, "f" * 64), self.marker + PREFIX + self.source + "\n"):
            report = copy.deepcopy(original)
            report["sysinfo"]["output"] = output
            self.image_report.write_text(json.dumps(report))
            with self.assertRaises(ValueError):
                self.check()

    def test_old_conflicting_and_missing_artifact_source(self):
        for data in (b"unmarked", (PREFIX + self.source + "\n" + PREFIX + "f" * 64 + "\n").encode(),
                     self.marker.replace("SOURCE_PROFILE_V1", "SOURCE_PROFILE_V2").encode()):
            self.image.write_bytes(data)
            with self.assertRaises(ValueError):
                artifact_source(self.image)

    def test_host_and_conformance_failures(self):
        for text in ("", "59 passed, 0 failed\n", self.host.read_text() + "FAILED (errors=1)\n",
                     self.host_text.replace(self.source, "f" * 64)):
            self.host.write_text(text)
            with self.assertRaises(ValueError):
                self.check()
        self.host.write_text(self.host_text)
        self.conformance.write_text("threshold gate: FAIL\n")
        with self.assertRaises(ValueError):
            self.check()

    def test_duplicate_json_keys_and_invalid_scope(self):
        for day in (True, 0, 1000, 3):
            with self.assertRaises(ValueError):
                self.check(day)
        self.image_report.write_text('{"passed":true,"passed":false}')
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.check()

    def test_source_set_changes_and_unrelated_files(self):
        (self.root / "notes.md").write_text("release notes")
        self.assertEqual(source_fingerprint(self.root), self.source)
        (self.root / "kernel/policy.h").write_text("#define WEIGHT 10\n")
        self.assertNotEqual(source_fingerprint(self.root), self.source)

    def test_generation_keeps_unchanged_header_timestamp(self):
        header = self.root / "generated/identity.h"
        command = [sys.executable, str(ROOT / "tools/source_identity.py"), "--root", str(self.root), "--header", str(header)]
        subprocess.run(command, check=True)
        before = header.stat().st_mtime_ns
        subprocess.run(command, check=True)
        self.assertEqual(header.stat().st_mtime_ns, before)
        (self.root / "kernel/main.c").write_text("int policy = 3;\n")
        subprocess.run(command, check=True)
        self.assertNotIn(self.source, header.read_text())


if __name__ == "__main__":
    unittest.main()
