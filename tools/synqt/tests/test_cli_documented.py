# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Every subcommand the CLI has appears in the CLI reference, and nothing else does.

Written after `docs/build-system-and-cli.md` spent a long time describing a `synqt new`
that "asks a short, security relevant set of questions" and a getting-started page that
listed three of them. The CLI has never prompted. Prose drifts silently; asking the
parser is the only way this stays true, so the page is checked against
`build_parser()` rather than against a second hand-maintained list.

The reverse direction matters as much: a command in the docs that no longer exists sends
a reader to a command that errors out, which is worse than an undocumented one.
"""

import re
import unittest
from pathlib import Path

from synqt import cli

DOCS = Path(__file__).resolve().parents[3] / "docs" / "build-system-and-cli.md"

# `--version` is an option on the top-level parser rather than a subcommand, and it is in
# the reference. Listing it here keeps the page honest without pretending it is a verb.
_NOT_SUBCOMMANDS = {"--version"}


def _parser_subcommands():
    """Every top-level subcommand argparse knows about, and every `parent child` pair."""
    found = set()
    for action in cli.build_parser()._actions:
        if action.__class__.__name__ != "_SubParsersAction":
            continue
        for name, sub in action.choices.items():
            found.add(name)
            for nested in sub._actions:
                if nested.__class__.__name__ != "_SubParsersAction":
                    continue
                for child in nested.choices:
                    found.add(f"{name} {child}")
    return found


def _documented_commands(text):
    """The `synqt <command>` invocations shown in the reference's command block."""
    block = re.search(r"^## The `synqt` command line tool\n+```cli\n(.*?)^```", text,
                      re.MULTILINE | re.DOTALL)
    assert block, "the CLI reference no longer has one command block to check"
    found = set()
    for line in block.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        match = re.match(r"^synqt ([a-z-]+|--version)(?: ([a-z-]+))?", line)
        if not match:
            continue
        head, tail = match.group(1), match.group(2)
        if head in _NOT_SUBCOMMANDS:
            found.add(head)
            continue
        found.add(head)
        # `synqt mesh ...` stands in for the whole mesh family, which has its own
        # section; a literal "..." is not a child command.
        if tail and not tail.startswith("-"):
            found.add(f"{head} {tail}")
    return found


class CliReferenceTest(unittest.TestCase):
    def setUp(self):
        self.text = DOCS.read_text(encoding="utf-8")
        self.documented = _documented_commands(self.text)
        self.real = _parser_subcommands()

    def test_every_top_level_command_is_documented(self):
        missing = {c for c in self.real if " " not in c} - self.documented
        self.assertFalse(missing, f"undocumented in {DOCS.name}: {sorted(missing)}")

    def test_no_documented_command_is_imaginary(self):
        # A `mesh <sub>` shown in the block must exist; the block's own "mesh ..." line
        # contributes only "mesh", which does.
        imaginary = self.documented - self.real - _NOT_SUBCOMMANDS
        self.assertFalse(imaginary, f"documented but not a command: {sorted(imaginary)}")

    def test_the_mesh_family_is_documented_somewhere_on_the_page(self):
        # The command block delegates it, so check the page carries the real names.
        for child in sorted(c for c in self.real if c.startswith("mesh ")):
            self.assertIn(f"synqt {child}", self.text)

    def test_the_page_does_not_claim_the_cli_prompts_outside_create(self):
        # `synqt create` is the only command that reads the terminal. Any other claim of
        # asking is the defect this file exists for.
        for match in re.finditer(r"`synqt (new|dev|build|add [a-z-]+)`[^.]{0,60}\basks\b",
                                 self.text):
            self.fail(f"the reference says a non-interactive command asks: {match.group(0)!r}")


if __name__ == "__main__":
    unittest.main()
