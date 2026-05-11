# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`.dev-identities`: what survives the reading, and what is said about what does not."""

import tempfile
import unittest
from pathlib import Path

from synqt import devidentities

SCOPES = ["anonymous", "user", "moderator", "admin"]


def write(text):
    directory = Path(tempfile.mkdtemp())
    (directory / devidentities.FILE_NAME).write_text(text)
    return directory


class ReadTest(unittest.TestCase):
    def test_no_file_is_not_a_problem(self):
        # Every project that has never heard of the feature is this case, so it must be
        # silent rather than "you have no identities".
        entries, problems = devidentities.read(Path(tempfile.mkdtemp()), SCOPES)
        self.assertEqual(entries, [])
        self.assertEqual(problems, [])

    def test_a_good_file_reads_in_order(self):
        directory = write("- email: alice@example.com\n  scope: admin\n"
                          "- email: bob@example.com\n  scope: user\n")
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [("admin", "alice@example.com"),
                                   ("user", "bob@example.com")])
        self.assertEqual(problems, [])

    def test_an_undeclared_scope_is_dropped_and_the_rest_survives(self):
        # The acceptance criterion for the whole file: one typo costs one entry, never the
        # picker. A development convenience that can take the sign-in down is worse than no
        # development convenience.
        directory = write("- email: alice@example.com\n  scope: admin\n"
                          "- email: bob@example.com\n  scope: wizard\n")
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [("admin", "alice@example.com")])
        self.assertEqual(len(problems), 1)
        self.assertIn("wizard", problems[0])
        self.assertIn("bob@example.com", problems[0])
        # And it names what it could have been, because the reader of this sentence is
        # about to retype the scope.
        self.assertIn("moderator", problems[0])

    def test_a_missing_field_is_named_by_its_line(self):
        directory = write("- email: alice@example.com\n- scope: user\n")
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [])
        self.assertEqual(len(problems), 2)
        self.assertIn("entry 1", problems[0])
        self.assertIn("entry 2", problems[1])

    def test_an_address_with_a_newline_is_refused(self):
        # It becomes a command-line value, part of a synthesized identity, and text on a
        # page. Refused once here rather than defended three times downstream.
        directory = write('- email: "alice@example.com\\nX-Evil: 1"\n  scope: user\n')
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [])
        self.assertEqual(len(problems), 1)

    def test_a_file_that_is_not_a_list_is_reported_whole(self):
        directory = write("alice@example.com: admin\n")
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("list of entries", problems[0])

    def test_broken_yaml_is_reported_and_not_raised(self):
        directory = write("- email: [unclosed\n")
        entries, problems = devidentities.read(directory, SCOPES)
        self.assertEqual(entries, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("not valid YAML", problems[0])


class ArgumentTest(unittest.TestCase):
    def test_the_scope_comes_first_so_the_separator_is_unambiguous(self):
        # An address can hold anything but a space. A scope name cannot hold an `=`. Putting
        # the scope first makes the FIRST `=` the separator, which is what the edge splits
        # on.
        flags = devidentities.arguments([("admin", "a=b@example.com")], [])
        self.assertEqual(flags, ["--dev-identity=admin=a=b@example.com"])

    def test_problems_ride_along_to_the_page(self):
        flags = devidentities.arguments([], ["entry 2 (bob) names scope 'wizard'"])
        self.assertEqual(flags, ["--dev-identity-problem=entry 2 (bob) names scope 'wizard'"])


class ProjectTest(unittest.TestCase):
    def test_the_project_vocabulary_is_what_entries_are_checked_against(self):
        directory = write("- email: alice@example.com\n  scope: operator\n")
        config = {"scopes": {"order": ["anonymous", "operator"]}}
        flags, problems = devidentities.for_project(directory, config)
        self.assertEqual(flags, ["--dev-identity=operator=alice@example.com"])
        self.assertEqual(problems, [])

        # The same file against a project that never declared `operator`.
        flags, problems = devidentities.for_project(directory,
                                                    {"scopes": {"order": ["anonymous"]}})
        self.assertEqual(flags, [f"--dev-identity-problem={problems[0]}"])


if __name__ == "__main__":
    unittest.main()
