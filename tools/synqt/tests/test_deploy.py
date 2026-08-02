# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The opt-in platform deploy step (`synqt build --deploy`).

The command each platform runs is asserted here rather than the result of running it: only one
of the three tools exists on any given machine, so a test that ran them would assert nothing
anywhere except the platform it happened to run on. tests/desktop-client/ runs the real thing
on whichever platform it is executed on.
"""

import json
import shutil
import struct
import subprocess
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

    def test_windows_refuses_when_there_is_no_executable(self):
        self._tool("windeployqt")
        with self.assertRaises(deploy.DeployError) as caught:
            deploy.deploy_client(self.root, "client", self.out, self.resolved, "windows")
        self.assertIn("no executable", str(caught.exception))

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


def _elf64(needed, *, strtab_offset=512, dynamic_offset=256, total=1024):
    """A minimal but genuinely well-formed ELF64 carrying the given DT_NEEDED sonames.

    Synthesised rather than taken from the host, because the parser has to be tested on the
    three platforms this suite runs on and only one of them has ELF files lying around.
    """
    table = b"\0" + b"".join(soname.encode() + b"\0" for soname in needed)
    indices = []
    position = 1
    for soname in needed:
        indices.append(position)
        position += len(soname) + 1

    entries = b"".join(struct.pack("<qQ", 1, index) for index in indices)
    entries += struct.pack("<qQ", 5, strtab_offset)  # DT_STRTAB
    entries += struct.pack("<qQ", 0, 0)              # DT_NULL

    header = b"\x7fELF\x02\x01\x01" + bytes(9)
    header += struct.pack("<HHIQQQIHHHHHH",
                          3, 0x3E, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    # One PT_LOAD mapping the whole file at vaddr 0, so a virtual address and a file offset
    # are the same number here, and one PT_DYNAMIC pointing at the table built above.
    program = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, total, total, 0x1000)
    program += struct.pack("<IIQQQQQQ", 2, 6, dynamic_offset, dynamic_offset, dynamic_offset,
                           len(entries), len(entries), 8)

    data = bytearray(total)
    data[0:len(header)] = header
    data[64:64 + len(program)] = program
    data[dynamic_offset:dynamic_offset + len(entries)] = entries
    data[strtab_offset:strtab_offset + len(table)] = table
    return bytes(data)


def _elf32(needed, *, strtab_offset=512, dynamic_offset=256, total=1024):
    """The same file in the 32-bit layout, whose header and tables are shaped differently."""
    table = b"\0" + b"".join(soname.encode() + b"\0" for soname in needed)
    indices = []
    position = 1
    for soname in needed:
        indices.append(position)
        position += len(soname) + 1

    entries = b"".join(struct.pack("<iI", 1, index) for index in indices)
    entries += struct.pack("<iI", 5, strtab_offset)
    entries += struct.pack("<iI", 0, 0)

    header = b"\x7fELF\x01\x01\x01" + bytes(9)
    # e_phoff is 52 (where the program headers are written below), not 28, which is merely
    # where the e_phoff field itself sits.
    header += struct.pack("<HHIIIIIHHHHHH", 3, 0x03, 1, 0, 52, 0, 0, 52, 32, 2, 0, 0, 0)
    program = struct.pack("<IIIIIIII", 1, 0, 0, 0, total, total, 5, 0x1000)
    program += struct.pack("<IIIIIIII", 2, dynamic_offset, dynamic_offset, dynamic_offset,
                           len(entries), len(entries), 6, 8)

    data = bytearray(total)
    data[0:len(header)] = header
    data[52:52 + len(program)] = program
    data[dynamic_offset:dynamic_offset + len(entries)] = entries
    data[strtab_offset:strtab_offset + len(table)] = table
    return bytes(data)


class DynamicNeedsTest(unittest.TestCase):
    """Reading DT_NEEDED out of the file, which is what the closure walk is built on."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_reads_the_sonames_in_order(self):
        path = self.root / "libthing.so"
        path.write_bytes(_elf64(["libQt6Gui.so.6", "libQt6Core.so.6", "libc.so.6"]))
        self.assertEqual(deploy._dynamic_needs(path),
                         ["libQt6Gui.so.6", "libQt6Core.so.6", "libc.so.6"])

    def test_a_file_with_no_dependencies_is_empty_not_an_error(self):
        path = self.root / "libbare.so"
        path.write_bytes(_elf64([]))
        self.assertEqual(deploy._dynamic_needs(path), [])

    def test_non_elf_files_are_skipped_silently(self):
        # Callers hand this every file in a plugin directory, and those hold qmldir files,
        # .qmlc caches and images alongside the libraries.
        for name, content in (("qmldir", b"module QtQuick\n"),
                              ("icon.png", b"\x89PNG\r\n\x1a\n"),
                              ("truncated.so", b"\x7fELF\x02\x01")):
            with self.subTest(name=name):
                path = self.root / name
                path.write_bytes(content)
                self.assertEqual(deploy._dynamic_needs(path), [])

    def test_a_missing_file_is_not_an_exception(self):
        self.assertEqual(deploy._dynamic_needs(self.root / "absent.so"), [])

    def test_reads_the_32_bit_layout_too(self):
        # A 32-bit ELF puts e_phoff, the program headers and the dynamic entries at different
        # offsets and widths. Qt ships 32-bit Linux and ARM kits, so this is a real target and
        # not a hypothetical one.
        path = self.root / "lib32.so"
        path.write_bytes(_elf32(["libQt6Core.so.6", "libm.so.6"]))
        self.assertEqual(deploy._dynamic_needs(path), ["libQt6Core.so.6", "libm.so.6"])

    def test_a_file_with_no_dynamic_segment_is_empty(self):
        # A statically linked binary has no PT_DYNAMIC at all, so there is nothing to ship
        # for it and nothing here should raise.
        path = self.root / "static"
        body = bytearray(_elf64(["libQt6Core.so.6"]))
        body[64 + 56:64 + 56 + 4] = struct.pack("<I", 0)  # PT_NULL over the PT_DYNAMIC entry
        path.write_bytes(bytes(body))
        self.assertEqual(deploy._dynamic_needs(path), [])

    def test_a_string_table_outside_the_mapped_segments_is_empty(self):
        path = self.root / "libodd.so"
        path.write_bytes(_elf64(["libQt6Core.so.6"], strtab_offset=0x400000))
        self.assertEqual(deploy._dynamic_needs(path), [])


class LinuxLayoutTest(unittest.TestCase):
    """The portable Linux layout: what it ships, and what it refuses to leave out.

    The case this exists for is the one the previous implementation got wrong. It copied the
    client binary's own dependencies and nothing else, so the platform plugin's Qt6XcbQpa and
    the Controls style's Qt6QuickControls2Impl were never shipped. Both are loaded at run time,
    so the tree started perfectly on any machine that already had Qt and on no other.
    """

    # What each file in the fake kit links, by file name. Only the plugin and the QML module
    # pull the two libraries the binary does not.
    NEEDS = {
        "client": ["libQt6Core.so.6", "libQt6Gui.so.6", "libQt6Network.so.6"],
        "libqxcb.so": ["libQt6XcbQpa.so.6", "libQt6Gui.so.6"],
        "libcontrolsplugin.so": ["libQt6QuickControls2Impl.so.6"],
        "libQt6XcbQpa.so.6": ["libQt6Gui.so.6"],
        "libQt6Core.so.6": ["libicuuc.so.73", "libc.so.6"],
    }

    LIBRARIES = ("libQt6Core.so.6", "libQt6Gui.so.6", "libQt6Network.so.6",
                 "libQt6XcbQpa.so.6", "libQt6QuickControls2Impl.so.6", "libicuuc.so.73")

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.out = self.root / "build" / "client-desktop" / "linux"
        self.out.mkdir(parents=True)
        self.kit = self.root / "qt"
        (self.out / "client").write_bytes(b"\x7fELF")

        for name in self.LIBRARIES:
            self._file(self.kit / "lib" / name)
        for module in ("QtQuick", "QtQuick/Controls", "QtQuick/VirtualKeyboard"):
            self._file(self.kit / "qml" / module / "qmldir", b"module x\n")
        self._file(self.kit / "qml" / "QtQuick" / "libqtquick2plugin.so")
        self._file(self.kit / "qml" / "QtQuick" / "Controls" / "libcontrolsplugin.so")
        self._file(self.kit / "qml" / "QtQuick" / "Controls" / "images" / "button.png", b"x")
        self._file(self.kit / "qml" / "QtQuick" / "VirtualKeyboard" / "libvkb.so")
        self._file(self.kit / "plugins" / "platforms" / "libqxcb.so")
        self._file(self.kit / "plugins" / "tls" / "libqopensslbackend.so")
        self._file(self.kit / "plugins" / "sqldrivers" / "libqsqlite.so")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _file(self, path, content=b"\x7fELF"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def _deploy(self, modules=("QtQuick", "QtQuick/Controls")):
        needs = self.NEEDS
        with mock.patch.object(deploy, "_qml_modules", return_value=list(modules)), \
             mock.patch.object(deploy, "_dynamic_needs",
                               side_effect=lambda path: needs.get(Path(path).name, [])):
            return deploy._deploy_linux(self.root, "client", self.out, str(self.kit))

    def test_ships_the_libraries_only_a_plugin_or_a_qml_module_needs(self):
        self._deploy()
        shipped = {entry.name for entry in (self.out / "lib").iterdir()}
        # Neither of these appears anywhere in the client binary's own dependency list.
        self.assertIn("libQt6XcbQpa.so.6", shipped)
        self.assertIn("libQt6QuickControls2Impl.so.6", shipped)

    def test_follows_the_closure_through_a_shipped_library(self):
        self._deploy()
        shipped = {entry.name for entry in (self.out / "lib").iterdir()}
        # Reached only through libQt6Core, so a one-level walk misses it.
        self.assertIn("libicuuc.so.73", shipped)

    def test_leaves_the_hosts_own_libraries_alone(self):
        self._deploy()
        # glibc is the host's to provide; shipping it is how a portable layout becomes a
        # tree that crashes on a host with a different loader.
        self.assertFalse((self.out / "lib" / "libc.so.6").exists())

    def test_ships_only_the_qml_modules_the_client_imports(self):
        self._deploy()
        self.assertTrue((self.out / "qml" / "QtQuick" / "Controls" / "qmldir").exists())
        # A nested module that was never imported. Copying QtQuick recursively would take it,
        # and with it the rest of a 200 MB tree.
        self.assertFalse((self.out / "qml" / "QtQuick" / "VirtualKeyboard").exists())
        # A module's own data directory is not a module and has to travel with it.
        self.assertTrue((self.out / "qml" / "QtQuick" / "Controls" / "images"
                         / "button.png").exists())

    def test_ships_the_plugin_directories_the_linked_modules_can_load(self):
        self._deploy()
        self.assertTrue((self.out / "plugins" / "platforms" / "libqxcb.so").exists())
        self.assertTrue((self.out / "plugins" / "tls").is_dir())
        # Nothing links Qt Sql, so no database driver can ever be asked for.
        self.assertFalse((self.out / "plugins" / "sqldrivers").exists())

    def test_the_launcher_uses_the_qt6_import_variable(self):
        self._deploy()
        body = (self.out / "client.sh").read_text()
        self.assertIn("LD_LIBRARY_PATH", body)
        self.assertIn("QT_PLUGIN_PATH", body)
        self.assertIn("QML_IMPORT_PATH", body)
        # QML2_IMPORT_PATH is the Qt 5 spelling, still honoured in 6.11 and documented as
        # deprecated. Writing it into a launcher generated today dates the output.
        self.assertNotIn("QML2_IMPORT_PATH", body)

    def test_a_second_deploy_refreshes_rather_than_keeping_what_is_there(self):
        self._deploy()
        stale = self.out / "qml" / "QtQuick" / "stale-from-an-older-kit.qml"
        stale.write_text("// left over\n")
        self._deploy()
        # Skipping qml/ and plugins/ when they already existed meant a re-deploy reported
        # shipping modules it had not touched, and the tree kept the previous kit's.
        self.assertFalse(stale.exists())

    def test_an_incomplete_closure_is_refused_rather_than_reported_as_success(self):
        # The guard against a future regression in the walk: on any machine that has Qt
        # installed the resulting tree still starts, so nothing else would notice.
        with mock.patch.object(deploy, "_library_closure", return_value={}):
            with self.assertRaises(deploy.DeployError) as caught:
                self._deploy()
        message = str(caught.exception)
        self.assertIn("would fail to start on a machine without Qt", message)
        # Named, with what needs it, so the report is actionable rather than a count.
        self.assertIn("libQt6QuickControls2Impl.so.6 (needed by libcontrolsplugin.so)", message)

    def test_the_note_counts_what_it_shipped(self):
        note = self._deploy()
        self.assertIn("6 Qt libraries", note)
        self.assertIn("2 QML modules", note)
        self.assertIn("2 plugin directories", note)

    def test_deploy_client_routes_linux_to_the_portable_layout(self):
        # Reached through the public entry point the CLI calls, not the private one the rest
        # of this class uses, so the dispatch is covered and not just the layout.
        needs = self.NEEDS
        with mock.patch.object(deploy, "_qml_modules", return_value=["QtQuick"]), \
             mock.patch.object(deploy, "_dynamic_needs",
                               side_effect=lambda path: needs.get(Path(path).name, [])):
            note = deploy.deploy_client(self.root, "client", self.out,
                                        {"host_qt": str(self.kit)}, "linux")
        self.assertIn("portable layout", note)

    def test_refuses_when_there_is_no_binary_to_deploy(self):
        (self.out / "client").unlink()
        with self.assertRaises(deploy.DeployError) as caught:
            self._deploy()
        self.assertIn("no executable", str(caught.exception))

    def test_refuses_without_a_kit_to_take_the_libraries_from(self):
        with self.assertRaises(deploy.DeployError) as caught:
            deploy._deploy_linux(self.root, "client", self.out, None)
        self.assertIn("host Qt kit", str(caught.exception))


class QmlModuleScanTest(unittest.TestCase):
    """Reading the import graph out of the kit's own qmlimportscanner."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.kit = self.root / "qt"
        (self.kit / "libexec").mkdir(parents=True)
        # The scanner lives in libexec, not bin, unlike macdeployqt and windeployqt.
        (self.kit / "libexec" / "qmlimportscanner").write_text("#!/bin/sh\n")
        for module in ("QtQuick", "QtQuick/Controls"):
            (self.kit / "qml" / module).mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _scan(self, stdout, returncode=0):
        result = subprocess.CompletedProcess([], returncode, stdout=stdout, stderr="")
        with mock.patch.object(deploy.subprocess, "run", return_value=result):
            return deploy._qml_modules(self.root, self.kit)

    def test_returns_the_module_paths_relative_to_the_kit(self):
        payload = json.dumps([
            {"name": "QtQuick", "relativePath": "QtQuick",
             "path": str(self.kit / "qml" / "QtQuick"), "type": "module"},
            {"name": "QtQuick.Controls", "relativePath": "QtQuick/Controls",
             "path": str(self.kit / "qml" / "QtQuick" / "Controls"), "type": "module"},
        ])
        self.assertEqual(self._scan(payload), ["QtQuick", "QtQuick/Controls"])

    def test_a_module_with_no_directory_is_skipped(self):
        # The client's own module is compiled into its resources, so the scanner reports it
        # with no path and there is nothing on disk to copy.
        payload = json.dumps([{"name": "SynQt", "type": "module"},
                              {"name": "QtQuick", "relativePath": "QtQuick",
                               "path": str(self.kit / "qml" / "QtQuick"), "type": "module"}])
        self.assertEqual(self._scan(payload), ["QtQuick"])

    def test_a_failed_scan_stops_the_deploy_instead_of_guessing(self):
        with self.assertRaises(deploy.DeployError) as caught:
            self._scan("", returncode=1)
        self.assertIn("qmlimportscanner failed", str(caught.exception))

    def test_the_scanner_is_found_in_libexec(self):
        self.assertEqual(deploy._tool(str(self.kit), "qmlimportscanner"),
                         self.kit / "libexec" / "qmlimportscanner")

    def test_output_that_is_not_json_is_a_clear_refusal(self):
        with self.assertRaises(deploy.DeployError) as caught:
            self._scan("not json at all")
        self.assertIn("not JSON", str(caught.exception))


class ToolFailureTest(unittest.TestCase):
    """What a platform tool's own failure looks like coming back out of `--deploy`."""

    def test_the_tools_own_diagnosis_is_what_reaches_the_caller(self):
        # macdeployqt names the specific framework or plugin it could not resolve, and that
        # line is the entire value of the message. A generic "deploy failed" would throw it
        # away and leave the developer to run the command by hand to find out why.
        failure = subprocess.CompletedProcess(
            ["/kit/bin/macdeployqt"], 1,
            stdout="", stderr="ERROR: Cannot resolve @rpath/QtFoo.framework/Versions/A/QtFoo\n")
        with mock.patch.object(deploy.subprocess, "run", return_value=failure):
            with self.assertRaises(deploy.DeployError) as caught:
                deploy._run(["/kit/bin/macdeployqt", "app"])
        message = str(caught.exception)
        self.assertIn("macdeployqt failed", message)
        self.assertIn("Cannot resolve @rpath/QtFoo.framework", message)

    def test_a_silent_failure_still_says_which_tool_failed(self):
        quiet = subprocess.CompletedProcess(["/kit/bin/windeployqt"], 3, stdout="", stderr="")
        with mock.patch.object(deploy.subprocess, "run", return_value=quiet):
            with self.assertRaises(deploy.DeployError) as caught:
                deploy._run(["/kit/bin/windeployqt", "app.exe"])
        self.assertIn("windeployqt failed", str(caught.exception))
        self.assertIn("(no output)", str(caught.exception))


class SigningChoiceTest(unittest.TestCase):
    """`--deploy` refuses to guess what you meant about signing."""

    def test_deploy_alone_is_refused_on_every_platform(self):
        for platform in ("macos", "windows", "linux"):
            with self.subTest(platform=platform):
                with self.assertRaises(deploy.DeployError) as caught:
                    deploy.check_signing_choice(platform, None, False)
                message = str(caught.exception)
                self.assertIn("--unsigned", message)
                # --sign is only offered where it is accepted. On Linux it is refused, so
                # naming it here would point the reader at the next error instead of a fix.
                self.assertEqual("--sign" in message, platform != "linux")

    def test_the_linux_refusal_does_not_send_the_reader_into_another_refusal(self):
        with self.assertRaises(deploy.DeployError) as first:
            deploy.check_signing_choice("linux", None, False)
        # Whatever the first message tells a Linux user to pass has to be accepted by the
        # very next command; --sign was not, and the pair read as a loop.
        self.assertNotIn("--sign <identity>", str(first.exception))
        deploy.check_signing_choice("linux", None, True)

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
