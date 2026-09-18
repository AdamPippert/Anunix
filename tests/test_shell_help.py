"""Keep shell discovery output aligned with the compiled dispatcher.

The test compiles the real manual, cmdlist, and help handlers in isolation, so
it never executes a command with side effects. Both normal and research builds
are checked because their command sets differ.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CATEGORIES = {
    "objects", "model", "network", "system", "workflow", "security",
    "shell", "interface",
}


class ShellHelpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "kernel/core/shell.c").read_text()
        docs_start = source.index("struct shell_man_entry {")
        docs_end = source.index("static void cmd_version(", docs_start)
        docs_code = source[docs_start:docs_end]
        dispatch_start = source.index("static void dispatch(")
        dispatch_end = source.index("/* --- Shell main loop --- */", dispatch_start)
        dispatch_code = source[dispatch_start:dispatch_end]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        cls.temp = tempfile.TemporaryDirectory(prefix="anunix-help-")
        cls.addClassCleanup(cls.temp.cleanup)
        fixture = Path(cls.temp.name) / "help.c"
        fixture.write_text(
            "#include <stdbool.h>\n#include <stdint.h>\n"
            "#include <stdio.h>\n#include <string.h>\n"
            "#define anx_strcmp strcmp\n#define anx_strlen strlen\n"
            "#define kprintf printf\n#define kputs(s) fputs((s), stdout)\n"
            + docs_code
            + "\nint main(int argc, char **argv) {\n"
            + "  if (argc > 1 && !strcmp(argv[1], \"--cmdlist\")) "
              "cmd_cmdlist(argc - 1, argv + 1);\n"
            + "  else if (argc > 1 && !strcmp(argv[1], \"--man\")) "
              "cmd_man(argc - 1, argv + 1);\n"
            + "  else cmd_help(argc, argv);\n"
            + "  return 0;\n}\n"
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

    @staticmethod
    def listed_commands(output):
        commands = []
        for line in output.splitlines():
            if line.startswith("  "):
                commands.extend(line.split())
        return commands

    def test_plain_help_is_short_and_points_to_unix_style_discovery(self):
        for binary, _ in self.variants.values():
            output = self.render(binary)
            self.assertLess(len(output), 700)
            self.assertIn("cmdlist [category]", output)
            self.assertIn("man <command>", output)
            self.assertNotIn("Commands (including aliases)", output)

    def test_cmdlist_lists_exactly_the_compiled_commands_once(self):
        for research, (binary, commands) in self.variants.items():
            with self.subTest(research=research):
                listed = self.listed_commands(self.render(binary, "--cmdlist"))
                self.assertEqual(set(listed), commands)
                self.assertEqual(len(listed), len(set(listed)))

    def test_every_compiled_command_has_a_manual_page(self):
        for research, (binary, commands) in self.variants.items():
            for command in commands:
                with self.subTest(research=research, command=command):
                    output = self.render(binary, "--man", command)
                    self.assertNotIn("no entry", output)
                    self.assertIn("NAME\n", output)
                    self.assertIn("SYNOPSIS\n", output)
                    self.assertIn(f"  {command} ", output)

    def test_category_filters_partition_the_command_set(self):
        for research, (binary, commands) in self.variants.items():
            documented = set()
            for category in CATEGORIES:
                output = self.render(binary, "--cmdlist", category)
                self.assertIn(f"\n{category}:\n", output)
                listed = self.listed_commands(output)
                self.assertTrue(listed, category)
                self.assertFalse(documented.intersection(listed))
                documented.update(listed)
            with self.subTest(research=research):
                self.assertEqual(documented, commands)

    def test_help_accepts_a_command_or_category(self):
        for binary, _ in self.variants.values():
            self.assertIn("SYNOPSIS\n  ping <ip>", self.render(binary, "ping"))
            self.assertIn("\nnetwork:\n", self.render(binary, "network"))

    def test_research_command_is_visible_only_in_research_build(self):
        normal, release_commands = self.variants[False]
        research, research_commands = self.variants[True]
        self.assertEqual(research_commands - release_commands, {"research-test"})
        self.assertNotIn("research-test", self.render(normal, "--cmdlist"))
        self.assertIn("research-test", self.render(research, "--cmdlist"))

    def test_unknown_names_return_actionable_errors(self):
        for binary, _ in self.variants.values():
            output = self.render(binary, "--man", "not-a-command")
            self.assertIn("man: no entry for 'not-a-command'", output)
            self.assertIn("cmdlist", output)
            output = self.render(binary, "--cmdlist", "not-a-category")
            self.assertIn("cmdlist: unknown category 'not-a-category'", output)
            for category in CATEGORIES:
                self.assertIn(category, output)


if __name__ == "__main__":
    unittest.main()
