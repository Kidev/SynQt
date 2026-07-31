# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt build` regenerates everything without rebuilding everything.

Regeneration is content-addressed: identical output leaves the file and its timestamp alone.
The explicit configure is skipped only when redundant, and never when the preset changed,
which the generated build graph does not watch.
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
        """A file in an unreadable encoding is overwritten, not compared."""
        target = self.root / "main.cpp"
        target.write_bytes(b"\xff\xfe binary")
        self.assertTrue(writer.write_if_changed(target, "clean\n"))
        self.assertEqual(target.read_text(), "clean\n")


class ConfigureSkipping(unittest.TestCase):
    """The guard around the explicit cmake configure."""

    def setUp(self):
        self._dir = TemporaryDirectory()
        self.root = Path(self._dir.name)
        self.build_dir = self.root / "build" / "host-debug"
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
        presets = self.root / "CMakePresets.json"
        presets.parent.mkdir(parents=True, exist_ok=True)
        presets.write_text(text)

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
        """A changed preset configures again: Ninja re-runs cmake for CMakeLists.txt but never
        reads CMakePresets.json.
        """
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
        """The stamp is written after the run, so a failed configure is retried."""
        def failing(command, cwd, verbose):
            raise RuntimeError("cmake failed")

        build._run = failing
        with self.assertRaises(RuntimeError):
            self.configure()
        self.assertFalse((self.build_dir / ".synqt-configure").exists())
        build._run = lambda command, cwd, verbose: self.runs.append(list(command))
        (self.build_dir / "CMakeCache.txt").write_text("")
        self.assertTrue(self.configure())


class IncompatibleCache(unittest.TestCase):
    """A build directory configured by another generator than the preset names is cleared:
    CMake refuses to reconfigure it ("Does not match the generator used previously").
    """

    def setUp(self):
        self._dir = TemporaryDirectory()
        self.root = Path(self._dir.name)
        self.build_dir = self.root / "build" / "host-debug"
        self.build_dir.mkdir(parents=True)
        (self.root / "generated").mkdir(parents=True, exist_ok=True)
        (self.root / "CMakePresets.json").write_text(json.dumps({
            "version": 6,
            "configurePresets": [
                {"name": "host", "generator": "Ninja"},
                {"name": "child", "inherits": "host"},
                {"name": "default"},
            ],
        }))

    def tearDown(self):
        self._dir.cleanup()

    def write_cache(self, generator):
        (self.build_dir / "CMakeCache.txt").write_text(
            f"CMAKE_GENERATOR:INTERNAL={generator}\nCMAKE_HOME_DIRECTORY:INTERNAL=/x\n")
        (self.build_dir / "CMakeFiles").mkdir(exist_ok=True)

    def clear(self, preset="host"):
        return build._clear_incompatible_cache(["cmake", "--preset", preset],
                                               self.build_dir, self.root)

    def test_a_mismatched_generator_clears_the_cache(self):
        self.write_cache("Unix Makefiles")
        note = self.clear()
        self.assertIn("Unix Makefiles", note)
        self.assertIn("Ninja", note)
        self.assertFalse((self.build_dir / "CMakeCache.txt").exists())
        self.assertFalse((self.build_dir / "CMakeFiles").exists())

    def test_a_matching_generator_is_left_alone(self):
        self.write_cache("Ninja")
        self.assertIsNone(self.clear())
        self.assertTrue((self.build_dir / "CMakeCache.txt").exists())

    def test_the_generator_is_followed_through_inherits(self):
        self.write_cache("Unix Makefiles")
        self.assertIsNotNone(self.clear("child"))

    def test_a_preset_naming_no_generator_never_clears(self):
        """A preset naming no generator never clears: the default is per platform."""
        self.write_cache("Unix Makefiles")
        self.assertIsNone(self.clear("default"))
        self.assertTrue((self.build_dir / "CMakeCache.txt").exists())

    def write_kit_cache(self, **values):
        lines = ["CMAKE_GENERATOR:INTERNAL=Ninja"]
        lines += [f"{key}:PATH={value}" for key, value in values.items()]
        (self.build_dir / "CMakeCache.txt").write_text("\n".join(lines) + "\n")
        (self.build_dir / "CMakeFiles").mkdir(exist_ok=True)

    def kit(self, version, name):
        path = self.root / "qt" / version / name
        (path / "bin").mkdir(parents=True, exist_ok=True)
        return path

    def test_a_tree_configured_against_another_wasm_kit_clears(self):
        """A tree configured against another wasm kit clears; its cached toolchain file names
        the old Qt.
        """
        old = self.kit("6.11.1", "wasm_singlethread")
        new = self.kit("6.12.0", "wasm_singlethread")
        host = self.kit("6.12.0", "gcc_64")
        self.write_kit_cache(
            CMAKE_TOOLCHAIN_FILE=old / "lib/cmake/Qt6/qt.toolchain.cmake",
            QT_HOST_PATH_CMAKE_DIR=self.root / "qt/6.11.1/gcc_64/lib/cmake")
        note = build._clear_incompatible_cache(
            [str(new / "bin" / "qt-cmake"), "-S", ".", "-B", str(self.build_dir),
             f"-DQT_HOST_PATH={host}"], self.build_dir, self.root)
        self.assertIsNotNone(note)
        self.assertIn("6.11.1", note)
        self.assertFalse((self.build_dir / "CMakeCache.txt").exists())

    def test_a_tree_configured_against_the_same_kit_is_left_alone(self):
        new = self.kit("6.12.0", "wasm_singlethread")
        host = self.kit("6.12.0", "gcc_64")
        self.write_kit_cache(
            CMAKE_TOOLCHAIN_FILE=new / "lib/cmake/Qt6/qt.toolchain.cmake",
            QT_HOST_PATH_CMAKE_DIR=host / "lib/cmake", Qt6_DIR="Qt6_DIR-NOTFOUND")
        self.assertIsNone(build._clear_incompatible_cache(
            [str(new / "bin" / "qt-cmake"), "-S", ".", "-B", str(self.build_dir),
             f"-DQT_HOST_PATH={host}"], self.build_dir, self.root))
        self.assertTrue((self.build_dir / "CMakeCache.txt").exists())

    def test_a_host_preset_that_moved_kit_clears_through_a_link(self):
        """Kits are compared by the real path behind the synqt/toolchain links."""
        old = self.kit("6.11.1", "gcc_64")
        new = self.kit("6.12.0", "gcc_64")
        link = self.root / "synqt" / "toolchain" / "qt" / "6.12.0" / "gcc_64"
        link.parent.mkdir(parents=True)
        link.symlink_to(new, target_is_directory=True)
        (self.root / "CMakePresets.json").write_text(json.dumps({
            "version": 6,
            "configurePresets": [
                {"name": "host", "generator": "Ninja", "cacheVariables": {
                    "CMAKE_PREFIX_PATH": "${sourceDir}/synqt/toolchain/qt/6.12.0/gcc_64"}},
                {"name": "host-release", "inherits": "host"},
            ],
        }))
        self.write_kit_cache(Qt6_DIR=new / "lib/cmake/Qt6")
        self.assertIsNone(self.clear("host-release"))
        self.write_kit_cache(Qt6_DIR=old / "lib/cmake/Qt6")
        self.assertIsNotNone(self.clear("host-release"))

    def test_an_unconfigured_directory_is_not_touched(self):
        self.assertIsNone(self.clear())

    def test_build_outputs_survive_the_clearing(self):
        """Only the cache and CMakeFiles go. This costs a reconfigure rather than a clean tree."""
        self.write_cache("Unix Makefiles")
        artifact = self.build_dir / "web"
        artifact.write_text("a built entity")
        self.clear()
        self.assertTrue(artifact.exists())

    def test_an_inherits_cycle_does_not_hang(self):
        (self.root / "CMakePresets.json").write_text(json.dumps({
            "version": 6,
            "configurePresets": [{"name": "a", "inherits": "b"},
                                 {"name": "b", "inherits": "a"}],
        }))
        self.write_cache("Unix Makefiles")
        self.assertIsNone(self.clear("a"))


if __name__ == "__main__":
    unittest.main()



class ClientBundleTargets(unittest.TestCase):
    """Which client entities `synqt build` assembles a bundle for, and where."""

    def test_every_client_entity_gets_its_own_bundle_directory(self):
        config = {"entities": [{"name": "web", "type": "web_edge",
                                "bundles": {"anonymous": "gate", "user": "app"}},
                               {"name": "app", "type": "client"},
                               {"name": "gate", "type": "client"}]}
        self.assertEqual(build.client_bundle_targets(config),
                         {"app": "build/client-app", "gate": "build/client-gate"})

    def test_a_single_client_project_keeps_build_client(self):
        config = {"entities": [{"name": "web", "type": "web_edge"},
                               {"name": "app", "type": "client"}]}
        self.assertEqual(build.client_bundle_targets(config), {"app": "build/client"})

    def test_a_desktop_only_client_is_not_assembled(self):
        # Nothing to serve. A desktop client produces an executable, not a bundle.
        config = {"entities": [{"name": "web", "type": "web_edge"},
                               {"name": "app", "type": "client"},
                               {"name": "kiosk", "type": "client",
                                "targets": ["desktop"]}]}
        self.assertNotIn("kiosk", build.client_bundle_targets(config))
