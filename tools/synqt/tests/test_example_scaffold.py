# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt examples` and `synqt new <name> --example <example>`: copying a shipped example.

The examples themselves are checked by test_examples.py. This is about the copy.

The quick start is one command away from a running system because of these, so what is
asserted here is the whole promise. The example the page names exists, a copy of it passes
`synqt check`, and the copy is a project rather than a directory somebody still has to
finish (a name of its own, a .gitignore, and no leftover of the machine it was copied from).
"""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import check, cli, examples


class ExamplesListingTest(unittest.TestCase):
    def test_every_example_is_a_project(self):
        found = examples.available()
        self.assertTrue(found)
        for name, _ in found:
            self.assertTrue((examples.root() / name / "synqt.yaml").is_file(), name)

    def test_the_example_the_quick_start_names_is_shipped(self):
        """docs/quick-start.md tells a first reader to copy this one, and a page that names
        an example the CLI does not carry is a page whose first command fails."""
        self.assertIn("stall", [name for name, _ in examples.available()])

    def test_each_one_says_what_it_is(self):
        for name, about in examples.available():
            self.assertTrue(about, f"{name} has no headline in its README title")

    def test_the_listing_names_them_and_says_how_to_start_one(self):
        printed = examples.listing()
        for name, about in examples.available():
            self.assertIn(name, printed)
            self.assertIn(about, printed)
        self.assertIn("--example", printed)


class ExampleScaffoldTest(unittest.TestCase):
    def _copy(self, name="shop", example="stall"):
        parent = Path(tempfile.mkdtemp())
        printed = examples.scaffold(parent, name, example)
        return parent / name, printed

    def test_a_copied_example_is_a_project_that_checks_out(self):
        root, printed = self._copy()
        self.assertIn("stall", printed)
        ok, messages = check.check_project(root)
        self.assertTrue(ok, messages)

    def test_the_copy_takes_the_name_it_was_given(self):
        root, _ = self._copy(name="shop")
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        self.assertEqual(config["project"]["name"], "shop")

    def test_the_commentary_in_the_example_survives_the_rename(self):
        """An example's synqt.yaml is commented throughout and is the first file a reader
        opens. Re-serialising it would hand them the same project with the explanations
        deleted."""
        root, _ = self._copy()
        text = (root / "synqt.yaml").read_text()
        self.assertIn("# The stall storefront", text)
        self.assertIn("name: shop", text)

    def test_the_copy_is_told_what_never_to_commit(self):
        """An example in the repository carries no .gitignore, because the repository has
        one. A copy is its own repository, and a mesh private key is one `git add -A` from
        being in it."""
        root, _ = self._copy()
        ignored = (root / ".gitignore").read_text()
        self.assertIn("synqt/mesh/*.key", ignored)
        self.assertIn("generated/", ignored)

    def test_nothing_from_the_machine_it_was_copied_from_comes_with_it(self):
        root, _ = self._copy()
        self.assertFalse((root / "CMakeUserPresets.json").exists()
                         and "stall" in (root / "CMakeUserPresets.json").read_text(),
                         "the example's own user preset was copied instead of regenerated")
        self.assertFalse((root / "build").exists())
        self.assertFalse((root / ".env").exists())

    def test_an_example_that_reads_a_secret_says_which_one(self):
        """gavel signs people in, so it will not start until a client secret is placed.
        Saying so in a file beats saying it in a stack trace on the first run."""
        root, printed = self._copy(name="auction", example="gavel")
        self.assertIn("GITHUB_CLIENT_SECRET=", (root / ".env.example").read_text())
        self.assertIn(".env.example", printed)

    def test_an_example_that_reads_no_secret_gets_an_empty_one(self):
        root, printed = self._copy()
        self.assertEqual((root / ".env.example").read_text().strip().count("\n"), 0)
        self.assertNotIn(".env.example", printed)

    def test_the_client_conveyance_warning_is_printed(self):
        """The client is served to every visitor whichever way the project started, so the
        GPLv3 reminder is not something `--example` gets to skip."""
        _, printed = self._copy()
        self.assertIn("GPLv3", printed)

    def test_an_unknown_example_names_the_ones_that_exist(self):
        parent = Path(tempfile.mkdtemp())
        with self.assertRaises(examples.ExampleError) as caught:
            examples.scaffold(parent, "shop", "storefront")
        self.assertIn("stall", str(caught.exception))

    def test_an_example_name_cannot_walk_out_of_the_examples_directory(self):
        parent = Path(tempfile.mkdtemp())
        for attempt in ("../src", "gavel/web", "/etc"):
            with self.assertRaises(examples.ExampleError):
                examples.scaffold(parent, "shop", attempt)

    def test_copying_over_a_project_that_is_already_there_is_refused(self):
        root, _ = self._copy()
        with self.assertRaises(Exception):
            examples.scaffold(root.parent, root.name, "stall")

    def test_an_empty_directory_is_taken_as_the_target(self):
        """`mkdir shop && cd shop` then scaffolding into it is how `synqt new` already
        behaves, and the two shapes should not differ."""
        parent = Path(tempfile.mkdtemp())
        (parent / "shop").mkdir()
        examples.scaffold(parent, "shop", "stall")
        self.assertTrue((parent / "shop" / "synqt.yaml").is_file())


class ExampleCommandLineTest(unittest.TestCase):
    def test_auth_and_example_together_are_refused(self):
        """An example has already decided whether it signs people in. Priming a provider
        into one would edit the file the reader is about to be told to read."""
        parent = tempfile.mkdtemp()
        self.assertEqual(cli.main(["new", "shop", "--example", "stall",
                                   "--auth", "github", "--parent-dir", parent]), 1)
        self.assertFalse((Path(parent) / "shop" / "synqt.yaml").exists())


if __name__ == "__main__":
    unittest.main()
