# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The multi-threaded WASM client under cross-origin isolation (CLIENT-2).

One knob (``build.client_threads``) drives the whole chain: the Qt WebAssembly kit, the
CMake preset, the edge's emitted COOP/COEP + worker-src headers, and the check that the
pairing holds. These tests pin that single source of truth end to end on the CLI side (the
edge's header emission itself is covered by the M5 web-edge C++ test).
"""

import json
import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import appgen, check, clientbuild, doctor, newproject, presets, toolchain


def _single():
    return {"project": {"name": "app"}, "security": {"cross_origin_isolation": False},
            "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]},
                         {"name": "web", "kind": "service", "capability": "web_edge"}]}


def _multi():
    config = _single()
    config["build"] = {"client_threads": "multi"}
    return config


class ResolveTest(unittest.TestCase):
    def test_default_is_single_threaded_not_isolated(self):
        config = _single()
        self.assertEqual(clientbuild.client_threads(config), "single")
        self.assertFalse(clientbuild.cross_origin_isolation(config))
        self.assertEqual(clientbuild.wasm_kit(config), "wasm_singlethread")

    def test_multi_threads_imply_isolation_and_the_multithread_kit(self):
        config = _multi()
        self.assertEqual(clientbuild.client_threads(config), "multi")
        self.assertTrue(clientbuild.cross_origin_isolation(config))
        self.assertEqual(clientbuild.wasm_kit(config), "wasm_multithread")

    def test_isolation_can_be_turned_on_without_threads(self):
        config = _single()
        config["security"]["cross_origin_isolation"] = True
        self.assertEqual(clientbuild.client_threads(config), "single")
        self.assertTrue(clientbuild.cross_origin_isolation(config))


class WasmBuildDirTest(unittest.TestCase):
    """The two kits must never share a build directory.

    qt-cmake picks the kit by injecting CMAKE_TOOLCHAIN_FILE, which CMake reads on the
    FIRST configure and caches forever. Pointed at a directory another kit configured, it
    silently keeps the old toolchain: flipping build.client_threads to multi then yields a
    single-threaded client while the edge advertises COOP/COEP. No error, no clue, and
    only a browser can tell. One directory per kit is what makes the knob real.
    """

    def test_each_kit_gets_its_own_build_dir(self):
        single = clientbuild.wasm_build_dir(_single())
        multi = clientbuild.wasm_build_dir(_multi())
        self.assertNotEqual(single, multi)
        self.assertIn("singlethread", single)
        self.assertIn("multithread", multi)

    def test_the_build_dir_is_under_build(self):
        self.assertTrue(clientbuild.wasm_build_dir(_single()).startswith("build/"))

    def test_the_preset_binary_dir_follows_the_kit(self):
        for config, kit in ((_single(), "singlethread"), (_multi(), "multithread")):
            with self.subTest(kit=kit):
                root = Path(tempfile.mkdtemp())
                presets.write(root, config)
                data = json.loads((root / "CMakePresets.json").read_text())
                wasm = next(p for p in data["configurePresets"] if p["name"] == "wasm")
                # The preset's binaryDir must agree with its toolchainFile, or driving
                # CMake through the preset reintroduces the stale-cache trap.
                self.assertIn(kit, wasm["binaryDir"])
                self.assertIn(clientbuild.wasm_build_dir(config), wasm["binaryDir"])


class PresetTest(unittest.TestCase):
    def test_single_uses_the_singlethread_kit_with_no_pthread_pool(self):
        root = Path(tempfile.mkdtemp())
        presets.write(root, _single())
        data = json.loads((root / "CMakePresets.json").read_text())
        wasm = next(p for p in data["configurePresets"] if p["name"] == "wasm")
        self.assertIn("wasm_singlethread", wasm["toolchainFile"])
        self.assertNotIn("QT_WASM_PTHREAD_POOL_SIZE", wasm["cacheVariables"])

    def test_multi_uses_the_multithread_kit_and_sizes_the_pool(self):
        root = Path(tempfile.mkdtemp())
        presets.write(root, _multi())
        data = json.loads((root / "CMakePresets.json").read_text())
        wasm = next(p for p in data["configurePresets"] if p["name"] == "wasm")
        self.assertIn("wasm_multithread", wasm["toolchainFile"])
        self.assertIn("QT_WASM_PTHREAD_POOL_SIZE", wasm["cacheVariables"])


class EdgeMainTest(unittest.TestCase):
    def _edge_main(self, config):
        edge = next(e for e in config["entities"] if e.get("capability") == "web_edge")
        return appgen.render_edge_main(config, edge)

    def test_single_edge_leaves_isolation_off(self):
        self.assertIn("config.crossOriginIsolation = false;", self._edge_main(_single()))

    def test_multi_edge_turns_isolation_on(self):
        # The generated edge flips crossOriginIsolation on, so WebEdge emits COOP/COEP and the
        # worker-src CSP entry the threaded loader needs (verified in the M5 C++ test).
        self.assertIn("config.crossOriginIsolation = true;", self._edge_main(_multi()))


class ValidationTest(unittest.TestCase):
    def test_an_invalid_threads_value_is_an_error(self):
        config = _single()
        config["build"] = {"client_threads": "many"}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("client_threads must be" in m for m in messages))

    def test_multi_with_isolation_off_warns_but_does_not_fail(self):
        # The scaffold writes cross_origin_isolation: false; opting into multi threads must
        # not break the build; it is auto-upgraded, with a warning.
        config = _multi()  # security.cross_origin_isolation is False
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertTrue(any(m.startswith("warn:") and "overridden to true" in m
                            for m in messages))

    def test_multi_with_isolation_on_is_clean(self):
        config = _multi()
        config["security"]["cross_origin_isolation"] = True
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertFalse(any("cross-origin" in m for m in messages))


class ToolchainAndDoctorTest(unittest.TestCase):
    def test_resolve_records_the_selected_kit(self):
        root = Path(tempfile.mkdtemp())
        self.assertEqual(toolchain.resolve(root, threads="multi")["wasm_kit"],
                         "wasm_multithread")
        self.assertEqual(toolchain.resolve(root, threads="single")["wasm_kit"],
                         "wasm_singlethread")

    def test_provision_hint_names_the_selected_kit_when_missing(self):
        # When the kit is not provisioned, the hint provisions the one the mode needs.
        hints = toolchain.provision_hints(
            {"wasm_kit": "wasm_multithread", "wasm_qt": None, "host_qt": "/x", "emcc": None})
        self.assertTrue(any("wasm_multithread" in h for h in hints))

    def test_doctor_reports_the_client_build_mode(self):
        parent = Path(tempfile.mkdtemp())
        newproject.scaffold(parent, "app")
        root = parent / "app"
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        config["build"] = {"client_threads": "multi"}
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        report = doctor.report(root)
        self.assertIn("multi-threaded WebAssembly", report)
        self.assertIn("cross-origin isolated", report)
        self.assertIn("wasm_multithread", report)


class ThreadOverrideTest(unittest.TestCase):
    """`synqt build --threads` overriding build.client_threads for one build. It has to move
    the whole chain, not just the kit: a threaded client served without cross-origin
    isolation gets no SharedArrayBuffer and quietly runs on one thread (pitfall 13).
    """

    def test_no_override_returns_the_config_untouched(self):
        config = _single()
        self.assertIs(clientbuild.with_threads(config, None), config)

    def test_override_moves_the_kit_the_build_dir_and_the_isolation_together(self):
        overridden = clientbuild.with_threads(_single(), "multi")
        self.assertEqual(clientbuild.client_threads(overridden), "multi")
        self.assertEqual(clientbuild.wasm_kit(overridden), "wasm_multithread")
        self.assertEqual(clientbuild.wasm_build_dir(overridden), "build/wasm-multithread")
        # Forced on despite security.cross_origin_isolation: false in _single().
        self.assertTrue(clientbuild.cross_origin_isolation(overridden))

    def test_override_can_also_force_single(self):
        multi = _single()
        multi["build"] = {"client_threads": "multi"}
        overridden = clientbuild.with_threads(multi, "single")
        self.assertEqual(clientbuild.wasm_kit(overridden), "wasm_singlethread")

    def test_override_does_not_mutate_the_callers_config(self):
        # The same dict is written back and read by the rest of the build; a one-off CLI
        # choice must not leak into it and look like a project setting.
        config = _single()
        clientbuild.with_threads(config, "multi")
        self.assertEqual(clientbuild.client_threads(config), "single")

    def test_a_bad_override_is_rejected(self):
        with self.assertRaises(ValueError):
            clientbuild.with_threads(_single(), "many")


if __name__ == "__main__":
    unittest.main()
