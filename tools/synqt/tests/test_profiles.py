# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a build profile means. The build type each environment resolves it to, whether the
binaries keep their symbols, and which directory the result lands in.

The table these pin is the answer to "what does --release do", and it is not
one answer. A visitor downloads the WebAssembly client over a network before a line of it
runs, and nobody downloads a service.
"""

import unittest

from synqt import profiles


class BuildTypeTest(unittest.TestCase):
    def test_release_is_size_optimised_for_the_browser_and_fast_for_a_service(self):
        self.assertEqual(profiles.build_type("release", "wasm"), "MinSizeRel")
        self.assertEqual(profiles.build_type("release", "host"), "Release")

    def test_debug_is_debug_everywhere(self):
        self.assertEqual(profiles.build_type("debug", "wasm"), "Debug")
        self.assertEqual(profiles.build_type("debug", "host"), "Debug")

    def test_custom_is_taken_literally_and_not_resolved_per_environment(self):
        # The escape hatch does no resolving. A user naming a build type gets that type in
        # every environment, or it would not be an escape hatch.
        self.assertEqual(profiles.build_type("custom", "wasm", "RelWithDebInfo"),
                         "RelWithDebInfo")
        self.assertEqual(profiles.build_type("custom", "host", "RelWithDebInfo"),
                         "RelWithDebInfo")

    def test_custom_refuses_a_type_cmake_does_not_have(self):
        # CMake does not warn about an unknown CMAKE_BUILD_TYPE; it silently supplies no
        # optimisation flags at all, so the refusal has to happen here.
        with self.assertRaises(ValueError) as caught:
            profiles.build_type("custom", "host", "Fast")
        self.assertIn("Fast", str(caught.exception))
        self.assertIn("MinSizeRel", str(caught.exception))

    def test_an_unknown_profile_or_environment_is_refused(self):
        with self.assertRaises(ValueError):
            profiles.build_type("fastest", "host")
        with self.assertRaises(ValueError):
            profiles.build_type("release", "toaster")


class StripTest(unittest.TestCase):
    def test_only_release_strips_unless_asked(self):
        self.assertTrue(profiles.strips("release"))
        self.assertFalse(profiles.strips("debug"))
        self.assertFalse(profiles.strips("custom"))

    def test_strip_can_be_asked_for_on_any_profile(self):
        self.assertTrue(profiles.strips("custom", strip=True))
        self.assertTrue(profiles.strips("debug", strip=True))


class BuildDirTest(unittest.TestCase):
    def test_each_profile_gets_its_own_directory(self):
        # Not tidiness. A release build and a development build compile different files, so
        # one directory would mean a full rebuild on every switch and a tree holding objects
        # the configuration it now has never asked for.
        self.assertEqual(profiles.build_dir("host", "debug"), "build/host-debug")
        self.assertEqual(profiles.build_dir("host", "release"), "build/host-release")

    def test_the_wasm_directory_is_keyed_to_the_kit_as_well(self):
        self.assertEqual(profiles.build_dir("wasm", "release", "wasm"), "build/wasm-release")
        self.assertEqual(profiles.build_dir("wasm", "debug", "wasm_multithread"),
                         "build/wasm-multithread-debug")

    def test_no_two_profiles_share_a_directory(self):
        for environment, kit in (("host", ""), ("wasm", "wasm"), ("wasm", "wasm_multithread")):
            directories = {profiles.build_dir(environment, profile, kit)
                           for profile in profiles.PROFILES}
            self.assertEqual(len(directories), len(profiles.PROFILES),
                             f"{environment}/{kit} reuses a directory across profiles")


if __name__ == "__main__":
    unittest.main()
