# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt build` regenerates everything, and that must not mean rebuilding everything.

Every build rewrites the CMake, the presets, each `main.cpp` and each resolved topology
from `synqt.yaml`, which is what keeps them from drifting. The cost was that `write_text`
moves a file's modification time whether or not a byte changed, and CMake and the compiler
read modification times: an unchanged `main.cpp` rewritten identically still bought a full
reconfigure and a full recompile of everything including it. Measured on the gavel example,
a no-op `synqt build` went from 4.76s to 0.08s once it stopped doing that.

Both halves are pinned here. The first is that regeneration is content-addressed: identical
output leaves the file alone. The second is that the explicit configure is skipped only when
it is genuinely redundant, and specifically that it is NOT skipped when the preset changed,
which is the one input the generated build graph does not watch for itself.
"""

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from synqt import build, writer


class WriteIfChanged(unittest.TestCase):
    def setUp(self):
        self._dir = TemporaryDirectory()
        self.root = Path(self._dir.name)

    def tearDown(self):
        self._dir.cleanup()

    def test_a_new_file_is_written(self):
        target = self.root / "main.cpp"
        self.assertTrue(writer.write_if_changed(target, "int main() {}\n"))
        self.assertEqual(target.read_text(), "int main() {}\n")

    def test_identical_content_leaves_the_file_alone(self):
        """The whole point: the modification time is what the build system reads."""
        target = self.root / "main.cpp"
        writer.write_if_changed(target, "same\n")
        before = target.stat().st_mtime_ns
        self.assertFalse(writer.write_if_changed(target, "same\n"))
        self.assertEqual(target.stat().st_mtime_ns, before)

    def test_changed_content_is_written(self):
        target = self.root / "main.cpp"
        writer.write_if_changed(target, "old\n")
        self.assertTrue(writer.write_if_changed(target, "new\n"))
        self.assertEqual(target.read_text(), "new\n")

    def test_a_missing_directory_is_created(self):
        target = self.root / "deep" / "nested" / "main.cpp"
        self.assertTrue(writer.write_if_changed(target, "x\n"))
        self.assertTrue(target.is_file())

    def test_an_unreadable_file_is_overwritten_rather_than_compared(self):
        """Something else wrote it, in an encoding this cannot read. Regenerating is the
        job; refusing because the old bytes are strange would leave stale output in place."""
        target = self.root / "main.cpp"
        target.write_bytes(b"\xff\xfe binary")
        self.assertTrue(writer.write_if_changed(target, "clean\n"))
        self.assertEqual(target.read_text(), "clean\n")


class ConfigureSkipping(unittest.TestCase):
    """The guard around the explicit cmake configure."""

    def setUp(self):
        self._dir = TemporaryDirectory()
        self.root = Path(self._dir.name)
        self.build_dir = self.root / "build" / "host"
        self.build_dir.mkdir(parents=True)
        self.runs = []
        self._real_run = build._run
        build._run = lambda command, cwd, verbose: self.runs.append(list(command))

    def tearDown(self):
        build._run = self._real_run
        self._dir.cleanup()

    def configure(self, command=("cmake", "--preset", "host")):
        return build._configure_if_needed(list(command), self.build_dir, self.root, False)

    def write_presets(self, text):
        (self.root / "CMakePresets.json").write_text(text)

    def test_the_first_build_configures(self):
        self.assertTrue(self.configure())
        self.assertEqual(len(self.runs), 1)

    def test_a_second_build_with_nothing_changed_does_not(self):
        self.configure()
        (self.build_dir / "CMakeCache.txt").write_text("")  # cmake would have written it
        self.assertFalse(self.configure())
        self.assertEqual(len(self.runs), 1)

    def test_a_different_command_configures_again(self):
        """A different Qt kit or edge URL must not silently inherit the old cache."""
        self.configure()
        (self.build_dir / "CMakeCache.txt").write_text("")
        self.assertTrue(self.configure(("cmake", "--preset", "host",
                                        "-DSYNQT_EDGE_URL=wss://other/sync")))

    def test_a_changed_preset_configures_again(self):
        """The case the generated build graph cannot catch: ninja re-runs cmake for a
        changed CMakeLists.txt and never looks at CMakePresets.json, which is read when
        cmake is invoked and carries the cache variables."""
        self.write_presets(json.dumps({"version": 6}))
        self.configure()
        (self.build_dir / "CMakeCache.txt").write_text("")
        self.assertFalse(self.configure())
        self.write_presets(json.dumps({"version": 6, "configurePresets": [{"name": "host"}]}))
        self.assertTrue(self.configure())

    def test_a_wiped_build_directory_configures_again(self):
        """`rm -rf build/host` has to mean what it looks like it means."""
        self.configure()
        (self.build_dir / "CMakeCache.txt").write_text("")
        self.assertFalse(self.configure())
        (self.build_dir / "CMakeCache.txt").unlink()
        self.assertTrue(self.configure())

    def test_a_failed_configure_is_retried_rather_than_remembered(self):
        """The stamp is written after the run, so a configure that raised leaves nothing
        behind claiming the directory is ready."""
        def failing(command, cwd, verbose):
            raise RuntimeError("cmake failed")

        build._run = failing
        with self.assertRaises(RuntimeError):
            self.configure()
        self.assertFalse((self.build_dir / ".synqt-configure").exists())
        build._run = lambda command, cwd, verbose: self.runs.append(list(command))
        (self.build_dir / "CMakeCache.txt").write_text("")
        self.assertTrue(self.configure())


if __name__ == "__main__":
    unittest.main()
