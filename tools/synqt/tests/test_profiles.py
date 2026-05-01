# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a build profile means: the build type per environment, whether binaries are stripped,
and the output directory. The client release is MinSizeRel, a service release Release.
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
        # `--custom` gives its build type to every environment.
        self.assertEqual(profiles.build_type("custom", "wasm", "RelWithDebInfo"),
                         "RelWithDebInfo")
        self.assertEqual(profiles.build_type("custom", "host", "RelWithDebInfo"),
                         "RelWithDebInfo")

    def test_custom_refuses_a_type_cmake_does_not_have(self):
        # An unknown CMAKE_BUILD_TYPE is refused; CMake would silently apply no flags.
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
        # Separate directories: release and development trees compile different files.
        self.assertEqual(profiles.build_dir("host", "debug"), "build/host-debug")
        self.assertEqual(profiles.build_dir("host", "release"), "build/host-release")

    def test_the_wasm_directory_is_keyed_to_the_kit_as_well(self):
        self.assertEqual(profiles.build_dir("wasm", "release", "wasm"), "build/wasm-release")
        self.assertEqual(profiles.build_dir("wasm", "debug", "wasm_multithread"),
                         "build/wasm-multithread-debug")

    def test_a_development_tree_says_so_in_its_name(self):
        # The `-dev` suffix is in the directory name.
        self.assertEqual(profiles.build_dir("host", "debug", dev_tools=True),
                         "build/host-debug-dev")
        self.assertNotEqual(profiles.build_dir("host", "debug", dev_tools=True),
                            profiles.build_dir("host", "debug"))

    def test_no_two_profiles_share_a_directory(self):
        for environment, kit in (("host", ""), ("wasm", "wasm"), ("wasm", "wasm_multithread")):
            directories = {profiles.build_dir(environment, profile, kit)
                           for profile in profiles.PROFILES}
            self.assertEqual(len(directories), len(profiles.PROFILES),
                             f"{environment}/{kit} reuses a directory across profiles")


if __name__ == "__main__":
    unittest.main()
