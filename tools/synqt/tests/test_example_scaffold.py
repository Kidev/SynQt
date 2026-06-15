# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt examples` and `synqt new <name> --example <example>`: copying a shipped example.

test_examples.py checks the examples themselves. Here: the example the quick start names
exists, a copy passes `synqt check`, and the copy is a finished project (its own name, a
.gitignore, nothing left from the source machine).
"""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import check, cli, examples, newproject


class ExamplesListingTest(unittest.TestCase):
    def test_every_example_is_a_project(self):
        found = examples.available()
        self.assertTrue(found)
        for name, _ in found:
            self.assertTrue((examples.root() / name / "synqt.yaml").is_file(), name)

    def test_the_example_the_quick_start_names_is_shipped(self):
        """The example docs/quick-start.md names is shipped."""
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
        """The comments in an example synqt.yaml survive the rename."""
        root, _ = self._copy()
        text = (root / "synqt.yaml").read_text()
        self.assertIn("# The stall storefront", text)
        self.assertIn("name: shop", text)

    def test_the_copy_is_told_what_never_to_commit(self):
        """The copy gets a .gitignore that keeps mesh keys out."""
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
        """gavel needs a client secret; the copy's .env.example names it."""
        root, printed = self._copy(name="auction", example="gavel")
        self.assertIn("GITHUB_CLIENT_SECRET=", (root / ".env.example").read_text())
        self.assertIn(".env.example", printed)

    def test_an_example_that_reads_no_secret_gets_an_empty_one(self):
        root, printed = self._copy()
        self.assertEqual((root / ".env.example").read_text().strip().count("\n"), 0)
        self.assertNotIn(".env.example", printed)

    def test_the_client_conveyance_warning_is_printed(self):
        """The client GPLv3 reminder is printed for `--example` too."""
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
        """An empty directory is taken as the target, as `synqt new` does."""
        parent = Path(tempfile.mkdtemp())
        (parent / "shop").mkdir()
        examples.scaffold(parent, "shop", "stall")
        self.assertTrue((parent / "shop" / "synqt.yaml").is_file())


class ExampleCommandLineTest(unittest.TestCase):
    def test_auth_and_example_together_are_refused(self):
        """`--auth` with `--example` is refused: the example decides its own sign-in."""
        parent = tempfile.mkdtemp()
        self.assertEqual(cli.main(["new", "shop", "--example", "stall",
                                   "--auth", "github", "--parent-dir", parent]), 1)
        self.assertFalse((Path(parent) / "shop" / "synqt.yaml").exists())


if __name__ == "__main__":
    unittest.main()


class ProjectNameFromAPathTest(unittest.TestCase):
    """`synqt new` takes a directory (`../shop`, `~/src/shop`, `.`); the project is named after
    its last component.
    """

    def test_a_new_project_given_a_path_is_named_after_its_directory(self):
        parent = Path(tempfile.mkdtemp())
        printed = newproject.scaffold(parent, str(parent / "nested" / "shop"))
        config = yaml.safe_load((parent / "nested" / "shop" / "synqt.yaml").read_text())
        self.assertEqual(config["project"]["name"], "shop")
        self.assertIn("Scaffolded 'shop'", printed)

    def test_an_example_copied_to_a_path_is_named_after_its_directory(self):
        parent = Path(tempfile.mkdtemp())
        examples.scaffold(parent, str(parent / "nested" / "shop"), "stall")
        config = yaml.safe_load((parent / "nested" / "shop" / "synqt.yaml").read_text())
        self.assertEqual(config["project"]["name"], "shop")

    def test_a_dot_names_the_directory_it_is_run_in(self):
        root = Path(tempfile.mkdtemp()) / "shop"
        root.mkdir()
        newproject.scaffold(root, ".")
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        self.assertEqual(config["project"]["name"], "shop")
