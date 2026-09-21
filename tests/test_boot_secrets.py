#!/usr/bin/env python3
"""Security invariants for the private bootstrap credential generator."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "gen_boot_secrets.py"


class BootSecretsTests(unittest.TestCase):
    def generate(self, content=None, mode=0o600, symlink=False):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            source = directory / "boot-secrets.conf"
            header = directory / "anx_boot_secrets.h"
            if content is not None:
                if symlink:
                    target = directory / "actual.conf"
                    target.write_text(content, encoding="utf-8")
                    os.chmod(target, mode)
                    source.symlink_to(target)
                else:
                    source.write_text(content, encoding="utf-8")
                    os.chmod(source, mode)
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "--input", str(source),
                 "--header", str(header)],
                text=True, capture_output=True, check=False,
            )
            generated = header.read_text(encoding="utf-8") if header.exists() else ""
            return result, generated

    def test_absent_config_generates_empty_public_table(self):
        result, generated = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define ANX_BOOT_SECRET_COUNT 0", generated)
        self.assertNotIn("jev-api-key", generated)
        self.assertNotIn("openai-api-key", generated)
        self.assertNotIn("anthropic-api-key", generated)

    def test_allowlisted_credentials_receive_expected_types(self):
        result, generated = self.generate(
            "wifi-ssid = network\n"
            "wifi-pass = passphrase\n"
            "jev-api-key = jev-example\n"
            "openai-api-key = openai-example\n"
            "anthropic-api-key = anthropic-example\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define ANX_BOOT_SECRET_COUNT 5", generated)
        self.assertEqual(generated.count("ANX_CRED_API_KEY"), 3)
        # Two Wi-Fi values and the terminating sentinel are opaque.
        self.assertEqual(generated.count("ANX_CRED_OPAQUE"), 3)

    def assert_rejected(self, content, expected):
        result, generated = self.generate(content)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(expected, result.stderr)
        self.assertEqual(generated, "")

    def test_rejects_unknown_duplicate_empty_long_and_non_ascii_values(self):
        cases = (
            ("other-key = value\n", "not an injectable name"),
            ("jev-api-key = one\njev-api-key = two\n", "duplicate injectable name"),
            ("openai-api-key =\n", "must not be empty"),
            ("anthropic-api-key = " + "x" * 256 + "\n", "exceeds 255 bytes"),
            ("jev-api-key = café\n", "must be ASCII"),
        )
        for content, expected in cases:
            with self.subTest(expected=expected):
                self.assert_rejected(content, expected)

    def test_rejects_group_or_world_readable_file(self):
        result, generated = self.generate("jev-api-key = value\n", mode=0o644)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mode must be 0600", result.stderr)
        self.assertEqual(generated, "")

    def test_rejects_symlink(self):
        result, generated = self.generate("jev-api-key = value\n", symlink=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing a symlinked secrets file", result.stderr)
        self.assertEqual(generated, "")


if __name__ == "__main__":
    unittest.main()
