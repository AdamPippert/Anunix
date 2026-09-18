"""Compare rendered shell help with the commands compiled into dispatch.

Compile the actual help handler in isolation so this check never executes a
command (not even `halt --help` or `ssh-keygen --help`, which have side effects).
Test both normal and research builds to catch conditional-command drift.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShellHelpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "kernel/core/shell.c").read_text()
        help_start = source.index("static void cmd_help(")
        help_end = source.index("static void cmd_version(", help_start)
        help_code = source[help_start:help_end]
        dispatch_start = source.index("static void dispatch(")
        dispatch_end = source.index("/* --- Shell main loop --- */", dispatch_start)
        dispatch_code = source[dispatch_start:dispatch_end]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        cls.temp = tempfile.TemporaryDirectory(prefix="anunix-help-")
        cls.addClassCleanup(cls.temp.cleanup)
        fixture = Path(cls.temp.name) / "help.c"
        fixture.write_text(
            "#include <stdio.h>\n#include <string.h>\n"
            "#define anx_strcmp strcmp\n#define kprintf printf\n"
            "#define kputs(s) fputs((s), stdout)\n"
            + help_code
            + "\nint main(int argc, char **argv) { cmd_help(argc, argv); return 0; }\n"
        )
        cls.variants = {}
        for research in (False, True):
            defines = ["-DANX_RESEARCH_TEST"] if research else []
            binary = Path(cls.temp.name) / ("help-research" if research else "help")
            subprocess.run(
                compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror"]
                + defines + [str(fixture), "-o", str(binary)], check=True,
                capture_output=True, text=True,
            )
            compiled_dispatch = subprocess.run(
                compiler + defines + ["-E", "-P", "-x", "c", "-"],
                input=dispatch_code, check=True, capture_output=True, text=True,
            ).stdout
            commands = set(re.findall(
                r'anx_strcmp\s*\(\s*argv\s*\[\s*0\s*\]\s*,\s*"([^"]+)"',
                compiled_dispatch,
            ))
            cls.variants[research] = (binary, commands)

    def render(self, binary, *args):
        return subprocess.run(
            [str(binary), *args], check=True, capture_output=True, text=True,
        ).stdout

    def topics(self, binary):
        summary = self.render(binary).split("Commands (including aliases):\n", 1)[0]
        return re.findall(r"^  help ([a-z]+)\s", summary, re.M)

    def test_plain_help_lists_exactly_the_compiled_commands(self):
        for research, (binary, commands) in self.variants.items():
            with self.subTest(research=research):
                self.assertGreater(len(commands), 1, "dispatch extraction is empty")
                output = self.render(binary)
                index = output.split("Commands (including aliases):\n", 1)[1].split()
                self.assertEqual(set(index), commands)
                self.assertEqual(len(index), len(set(index)), "duplicate index entries")

    def test_advertised_topics_cover_every_command_without_inventing_commands(self):
        for research, (binary, commands) in self.variants.items():
            with self.subTest(research=research):
                topics = self.topics(binary)
                self.assertTrue(topics, "no topics advertised")
                self.assertEqual(len(topics), len(set(topics)))
                documented = set()
                for topic in topics:
                    output = self.render(binary, topic)
                    self.assertNotIn("unknown topic", output)
                    entries = set(re.findall(r"^  ([a-z][a-z0-9_-]*|\?)\s", output, re.M))
                    self.assertTrue(entries, topic + " has no command entries")
                    documented.update(entries)
                self.assertEqual(documented, commands)

    def test_research_command_is_visible_only_in_research_build(self):
        normal, release_commands = self.variants[False]
        research, research_commands = self.variants[True]
        self.assertEqual(research_commands - release_commands, {"research-test"})
        self.assertNotIn("research-test", self.render(normal))
        self.assertNotIn("research-test", self.render(normal, "system"))
        self.assertIn("research-test", self.render(research))
        self.assertIn("research-test", self.render(research, "system"))

    def test_unknown_topic_returns_an_actionable_error(self):
        for binary, _ in self.variants.values():
            output = self.render(binary, "not-a-topic")
            self.assertIn("help: unknown topic 'not-a-topic'", output)
            for topic in self.topics(binary):
                self.assertIn(topic, output)


if __name__ == "__main__":
    unittest.main()
