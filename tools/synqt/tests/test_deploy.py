# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The opt-in platform deploy step (`synqt build --deploy`).

The command each platform runs is asserted here rather than the result of running it: only one
of the three tools exists on any given machine, so a test that ran them would assert nothing
anywhere except the platform it happened to run on. tests/desktop-client/ runs the real thing
on whichever platform it is executed on.
"""

import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from synqt import deploy


class DeployCommandTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.out = self.root / "build" / "client-desktop" / "here"
        self.out.mkdir(parents=True)
        self.kit = self.root / "qt"
        (self.kit / "bin").mkdir(parents=True)
        self.resolved = {"host_qt": str(self.kit)}

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _tool(self, name):
        path = self.kit / "bin" / name
        path.write_text("#!/bin/sh\n")
        return path

    def test_macos_runs_macdeployqt_on_the_bundle(self):
        (self.out / "client.app").mkdir()
        tool = self._tool("macdeployqt")
        with mock.patch.object(deploy, "_run", return_value="") as run:
            note = deploy.deploy_client(self.root, "client", self.out, self.resolved, "macos")
        command = run.call_args[0][0]
        self.assertEqual(command[0], str(tool))
        self.assertEqual(command[1], str(self.out / "client.app"))
        # -qmldir is what lets macdeployqt find the QML the app imports; without it the
        # deployed bundle loads and then fails at the first import.
        self.assertIn(f"-qmldir={self.root}", command)
        self.assertNotIn("-codesign", " ".join(command))
        self.assertIn("UNSIGNED", note)

    def test_macos_refuses_when_there_is_no_bundle(self):
        # The failure a bare Mach-O produced before cmakegen set MACOSX_BUNDLE: macdeployqt
        # accepts nothing else, so this has to say so rather than hand it a path it will reject.
        self._tool("macdeployqt")
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.deploy_client(self.root, "client", self.out, self.resolved, "macos")
        self.assertIn("no app bundle", str(caught.exception))

    def test_windows_runs_windeployqt_on_the_exe(self):
        (self.out / "client.exe").write_bytes(b"MZ")
        tool = self._tool("windeployqt")
        with mock.patch.object(deploy, "_run", return_value="") as run:
            deploy.deploy_client(self.root, "client", self.out, self.resolved, "windows")
        command = run.call_args[0][0]
        self.assertEqual(command[0], str(tool))
        self.assertIn("--qmldir", command)
        self.assertEqual(command[-1], str(self.out / "client.exe"))

    def test_linux_copies_only_the_kit_libraries_and_writes_a_launcher(self):
        (self.out / "client").write_bytes(b"\x7fELF")
        (self.kit / "lib").mkdir()
        (self.kit / "lib" / "libQt6Core.so.6").write_bytes(b"\x7fELF")
        (self.kit / "qml").mkdir()
        (self.kit / "plugins").mkdir()
        ldd = (f"\tlibQt6Core.so.6 => {self.kit}/lib/libQt6Core.so.6 (0x00007f)\n"
               "\tlibc.so.6 => /usr/lib/libc.so.6 (0x00007f)\n")
        with mock.patch.object(deploy, "_run", return_value=ldd):
            note = deploy._deploy_linux(self.root, "client", self.out, str(self.kit))
        # glibc is the host's to provide; shipping it is how a portable layout becomes a
        # tree that crashes on a host with a different loader.
        self.assertTrue((self.out / "lib" / "libQt6Core.so.6").exists())
        self.assertFalse((self.out / "lib" / "libc.so.6").exists())
        launcher = self.out / "client.sh"
        self.assertTrue(launcher.exists())
        body = launcher.read_text()
        self.assertIn("LD_LIBRARY_PATH", body)
        self.assertIn("QML2_IMPORT_PATH", body)
        self.assertIn("1 Qt libraries", note)

    def test_missing_tool_names_the_kit_it_looked_in(self):
        (self.out / "client.app").mkdir()
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.deploy_client(self.root, "client", self.out, self.resolved, "macos")
        self.assertIn(str(self.kit), str(caught.exception))

    def test_no_host_qt_is_a_clear_refusal(self):
        (self.out / "client.app").mkdir()
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.deploy_client(self.root, "client", self.out, {}, "macos")
        self.assertIn("host Qt kit", str(caught.exception))

    def test_macos_signs_through_macdeployqt(self):
        (self.out / "client.app").mkdir()
        self._tool("macdeployqt")
        with mock.patch.object(deploy, "_run", return_value="") as run:
            note = deploy.deploy_client(self.root, "client", self.out, self.resolved, "macos",
                                        sign="Developer ID Application: Acme (AB12CD34)")
        # -codesign, not a separate codesign call: the frameworks and plugins inside the
        # bundle each have to be signed before the bundle, and macdeployqt walks that tree.
        self.assertIn("-codesign=Developer ID Application: Acme (AB12CD34)", run.call_args[0][0])
        self.assertIn("notarize", note)

    def test_windows_signs_with_a_timestamp(self):
        (self.out / "client.exe").write_bytes(b"MZ")
        self._tool("windeployqt")
        with mock.patch.object(deploy, "_run", return_value="") as run:
            deploy.deploy_client(self.root, "client", self.out, self.resolved, "windows",
                                 sign="Acme Ltd")
        command = run.call_args[0][0]
        self.assertEqual(command[0], "signtool")
        self.assertIn("Acme Ltd", command)
        # Without a timestamp the signature dies with the certificate instead of outliving it.
        self.assertIn("/tr", command)


class SigningChoiceTest(unittest.TestCase):
    """`--deploy` refuses to guess what you meant about signing."""

    def test_deploy_alone_is_refused_on_every_platform(self):
        for platform in ("macos", "windows", "linux"):
            with self.subTest(platform=platform):
                with self.assertRaises(deploy.DeployError) as caught:
                    deploy.check_signing_choice(platform, None, False)
                message = str(caught.exception)
                self.assertIn("--sign", message)
                self.assertIn("--unsigned", message)

    def test_the_refusal_says_what_unsigned_costs_here(self):
        # The three platforms differ, and a single "unsigned is bad" would be wrong on two:
        # a Windows build runs unsigned, and Linux has no binary signing at all.
        with self.assertRaises(deploy.DeployError) as mac:
            deploy.check_signing_choice("macos", None, False)
        self.assertIn("Gatekeeper", str(mac.exception))
        with self.assertRaises(deploy.DeployError) as win:
            deploy.check_signing_choice("windows", None, False)
        self.assertIn("SmartScreen", str(win.exception))
        with self.assertRaises(deploy.DeployError) as lin:
            deploy.check_signing_choice("linux", None, False)
        self.assertIn("no binary code signing", str(lin.exception))

    def test_both_flags_together_is_refused(self):
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.check_signing_choice("macos", "Some Identity", True)
        self.assertIn("contradict", str(caught.exception))

    def test_signing_on_linux_is_refused_with_the_reason(self):
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.check_signing_choice("linux", "Some Identity", False)
        self.assertIn("nothing to do on Linux", str(caught.exception))

    def test_a_stated_choice_is_accepted(self):
        deploy.check_signing_choice("macos", "Developer ID Application: Acme", False)
        deploy.check_signing_choice("macos", None, True)
        deploy.check_signing_choice("linux", None, True)
        deploy.check_signing_choice("windows", "Acme Ltd", False)


if __name__ == "__main__":
    unittest.main()
