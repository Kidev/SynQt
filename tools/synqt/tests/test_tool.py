# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The toolchain: resolution, .wasm precompression, the process manifest, desktop layout."""

import gzip
import json
import os
import shutil
import tempfile
import unittest
import unittest.mock
from pathlib import Path

from synqt import appmodel, clientshell, cmakegen, maingen
from synqt import loadingpage
from synqt import build as buildmod
from synqt import newproject, toolchain
from synqt import run as runmod


class ToolchainTest(unittest.TestCase):
    def test_resolves_installed_kits_or_reports_hints(self):
        resolved = toolchain.resolve(tempfile.mkdtemp())
        self.assertEqual(resolved["qt_version"], "6.12.0")
        self.assertEqual(resolved["emscripten_version"], "5.0.5")
        # Resolves with a system Qt; otherwise the report gives the provisioning command.
        if not resolved["host_qt"]:
            self.assertTrue(any("aqt install-qt" in h for h in toolchain.provision_hints(resolved)))
        self.assertIn("Toolchain (Qt 6.12.0", toolchain.report(tempfile.mkdtemp()))

    def test_provision_hints_carry_the_coordinates_aqt_actually_accepts(self):
        # Hints are run as printed. The host hint uses the aqt arch (linux_gcc_64), per
        # platform; the WASM hint is all_os/wasm everywhere.
        cases = [("win32", "install-qt windows desktop 6.12.0 win64_msvc2022_64"),
                 ("darwin", "install-qt mac desktop 6.12.0 clang_64"),
                 ("linux", "install-qt linux desktop 6.12.0 linux_gcc_64")]
        for platform, expected in cases:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    hints = toolchain.provision_hints({"wasm_kit": "wasm_singlethread",
                                                       "wasm_qt": None, "host_qt": None,
                                                       "emcc": None})
                host = next(h for h in hints if "desktop" in h)
                wasm = next(h for h in hints if "wasm_singlethread" in h)
                self.assertIn(expected, host)
                self.assertIn("install-qt all_os wasm 6.12.0 wasm_singlethread", wasm)

    def test_host_kit_directory_is_the_hosts_own_never_a_hard_coded_linux_one(self):
        # The host kit directory differs per platform.
        for platform, expected in [("win32", "msvc2022_64"), ("darwin", "macos"),
                                   ("linux", "gcc_64"), ("freebsd14", "gcc_64")]:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    self.assertEqual(toolchain.host_kit_dir(), expected)

    def test_an_explicit_qtdir_wins_and_never_stands_in_for_the_wasm_kit(self):
        root = Path(tempfile.mkdtemp())
        qt = root / "Qt" / "6.12.0"
        (qt / "gcc_64" / "lib" / "cmake").mkdir(parents=True)
        (qt / "wasm_singlethread" / "lib" / "cmake").mkdir(parents=True)
        project = root / "project"
        project.mkdir()

        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}):
            resolved = toolchain.resolve(project)
            # QTDIR beats a system Qt.
            self.assertEqual(resolved["host_qt"], str(qt / "gcc_64"))
            # The WASM kit is found as QTDIR's sibling.
            self.assertEqual(resolved["wasm_qt"], str(qt / "wasm_singlethread"))

            # QTDIR is never returned as the WASM kit.
            shutil.rmtree(qt / "wasm_singlethread")
            self.assertNotEqual(toolchain.resolve(project)["wasm_qt"], str(qt / "gcc_64"))


    def _kit(self, modules, extra_kits=()):
        """A Qt installation on disk carrying exactly the named modules, as package config
        files: the files `find_package(Qt6 COMPONENTS Foo)` looks for.
        """
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        qt = root / "Qt" / toolchain.QT_VERSION
        for kit, names in list(modules.items()) + [(k, []) for k in extra_kits]:
            cmake = qt / kit / "lib" / "cmake"
            cmake.mkdir(parents=True, exist_ok=True)
            for name in names:
                (cmake / f"Qt6{name}").mkdir(parents=True, exist_ok=True)
                (cmake / f"Qt6{name}" / f"Qt6{name}Config.cmake").write_text("")
        project = root / "project"
        project.mkdir()
        return qt, project

    def test_a_kit_short_of_a_module_synqt_links_is_not_a_complete_toolchain(self):
        # A stock installation: aqt without -m installs only qtbase and qtdeclarative, and
        # no WebAssembly QtRemoteObjects exists prebuilt. Both kits are found, and
        # incomplete.
        qt, project = self._kit({"gcc_64": ["RemoteObjects", "WebSockets", "HttpServer"],
                                 "wasm_singlethread": ["WebSockets"]})
        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}):
            resolved = toolchain.resolve(project)
            self.assertEqual(resolved["host_qt_missing"], ["NetworkAuth"])
            self.assertEqual(resolved["wasm_qt_missing"], ["RemoteObjects"])
            self.assertFalse(toolchain.is_complete(resolved))
            # Complete for a service-only build, which needs no WASM kit; the host kit's own
            # gap still counts.
            self.assertFalse(toolchain.is_complete(resolved, need_wasm=False))

            # The report names each gap under its kit.
            report = toolchain.report(project)
            self.assertIn("missing module: Qt6RemoteObjects", report)
            self.assertIn("missing module: Qt6NetworkAuth", report)

    def test_a_complete_kit_is_reported_complete_and_asks_for_nothing(self):
        # A kit carrying every module is accepted with nothing to report.
        qt, project = self._kit({
            "gcc_64": ["RemoteObjects", "WebSockets", "HttpServer", "NetworkAuth"],
            "wasm_singlethread": ["RemoteObjects", "WebSockets"]})
        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}), \
                unittest.mock.patch.object(toolchain.shutil, "which",
                                           lambda name: "/usr/bin/" + name), \
                unittest.mock.patch.object(toolchain, "_emsdk",
                                           lambda project_dir: Path("/emcc")):
            resolved = toolchain.resolve(project)
            self.assertTrue(toolchain.is_complete(resolved))
            self.assertEqual(toolchain.provision_hints(resolved), [])

    def test_the_host_hint_installs_the_modules_synqt_links(self):
        # The host hint installs every module SynQt links.
        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"):
            hints = toolchain.provision_hints({"wasm_kit": "wasm_singlethread",
                                               "wasm_qt": None, "host_qt": None,
                                               "emcc": None})
        host = next(h for h in hints if "desktop" in h)
        for archive in ("qtremoteobjects", "qtwebsockets", "qthttpserver", "qtnetworkauth"):
            self.assertIn(archive, host)

    def test_the_wasm_remoteobjects_hint_is_a_source_build_never_an_aqt_module(self):
        # aqt has no WebAssembly qtremoteobjects; the hint builds it with the kit's qt-cmake
        # from the pinned source, naming QT_HOST_PATH.
        qt, project = self._kit({"gcc_64": [], "wasm_singlethread": ["WebSockets"]})
        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}):
            hints = toolchain.provision_hints(toolchain.resolve(project))

        wasm_module_hints = [h for h in hints if "install-qt all_os wasm" in h]
        self.assertFalse([h for h in wasm_module_hints if "qtremoteobjects" in h],
                         "offered qtremoteobjects as a WebAssembly aqt module")
        source = next(h for h in hints if "install-src" in h)
        self.assertIn("--archives qtremoteobjects", source)
        build = next(h for h in hints if "qt-cmake" in h)
        # Against the resolved kit, compared as posix paths as the hint prints them
        # (backslashes are escapes to shells and CMake).
        self.assertIn((qt / "wasm_singlethread" / "bin" / "qt-cmake").as_posix(), build)
        self.assertIn(f"QT_HOST_PATH={(qt / 'gcc_64').as_posix()}", build)
        self.assertIn(f"-DCMAKE_INSTALL_PREFIX={(qt / 'wasm_singlethread').as_posix()}", build)

    def test_adding_a_module_lands_in_the_kit_that_is_short_of_it(self):
        # aqt lays out <outputdir>/<version>/<kit>, so the hint names the kit's grandparent.
        qt, project = self._kit({"gcc_64": ["RemoteObjects", "WebSockets", "HttpServer"],
                                 "wasm_singlethread": ["RemoteObjects", "WebSockets"]})
        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}):
            hints = toolchain.provision_hints(toolchain.resolve(project))
        host = next(h for h in hints if "install-qt" in h)
        self.assertIn(f"-O {qt.parent.as_posix()}", host)


class EmscriptenPinTest(unittest.TestCase):
    """Qt for WebAssembly is built with one Emscripten, and a client linked by another may not
    run, so the resolver holds emcc to the pin.
    """

    def _emsdk(self, where: Path, version: str) -> Path:
        emcc = where / "upstream" / "emscripten" / "emcc"
        emcc.parent.mkdir(parents=True)
        emcc.write_text("#!/bin/sh\n")
        (emcc.parent / "emscripten-version.txt").write_text(f'"{version}"\n')
        return emcc

    def test_the_projects_emsdk_wins_over_another_version_on_path(self):
        project = Path(tempfile.mkdtemp())
        pinned = self._emsdk(project / "synqt" / "toolchain" / "emsdk",
                             toolchain.EMSCRIPTEN_VERSION)
        elsewhere = self._emsdk(Path(tempfile.mkdtemp()), "4.0.1")
        with unittest.mock.patch.object(toolchain.shutil, "which",
                                        lambda name: str(elsewhere) if name == "emcc" else None):
            resolved = toolchain.resolve(project)
        self.assertEqual(resolved["emcc"], str(pinned))
        self.assertEqual(resolved["emcc_version"], toolchain.EMSCRIPTEN_VERSION)

    def test_only_another_version_is_a_toolchain_that_cannot_build_the_client(self):
        project = Path(tempfile.mkdtemp())
        elsewhere = self._emsdk(Path(tempfile.mkdtemp()), "4.0.1")
        resolved = {"host_qt": "/qt", "wasm_qt": "/qt-wasm", "cmake": "/cmake",
                    "emcc": str(elsewhere), "emcc_version": "4.0.1",
                    "host_qt_missing": [], "wasm_qt_missing": []}
        self.assertFalse(toolchain.is_complete(resolved, need_wasm=True))
        self.assertTrue(toolchain.is_complete(resolved, need_wasm=False))
        hints = toolchain.provision_hints(resolved)
        self.assertTrue(any(f"emsdk install {toolchain.EMSCRIPTEN_VERSION}" in hint
                            and "4.0.1" in hint for hint in hints), hints)


class PrecompressTest(unittest.TestCase):
    def test_wasm_is_brotli_and_gzip_compressed(self):
        client = Path(tempfile.mkdtemp())
        payload = b"\x00asm" + b"x" * 4096
        (client / "app.wasm").write_bytes(payload)
        count = buildmod.precompress(client)
        self.assertEqual(count, 1)
        self.assertTrue((client / "app.wasm.gz").exists())
        self.assertEqual(gzip.decompress((client / "app.wasm.gz").read_bytes()), payload)
        # brotli is available in this environment.
        self.assertTrue((client / "app.wasm.br").exists())

    def test_every_critical_path_asset_is_compressed_not_only_the_wasm(self):
        # The Emscripten glue .js is compressed too.
        client = Path(tempfile.mkdtemp())
        (client / "client.wasm").write_bytes(b"\x00asm" + b"x" * 5000)
        (client / "client.js").write_text("// glue\n" + "x" * 5000)
        (client / "index.html").write_text("<!doctype html>" + "x" * 5000)
        (client / "synqt-manifest.json").write_text('{"pad":"' + "x" * 5000 + '"}')
        count = buildmod.precompress(client)
        self.assertEqual(count, 4)
        for name in ("client.wasm", "client.js", "index.html", "synqt-manifest.json"):
            self.assertTrue((client / (name + ".gz")).exists(), name)
            self.assertTrue((client / (name + ".br")).exists(), name)

    def test_an_unchanged_asset_is_not_compressed_again(self):
        # Variants written from the source as it stands are not recompressed.
        client = Path(tempfile.mkdtemp())
        asset = client / "client.wasm"
        asset.write_bytes(b"\x00asm" + b"x" * 5000)
        self.assertEqual(buildmod.precompress(client), 1)
        stamped = (client / "client.wasm.gz").stat().st_mtime_ns

        self.assertEqual(buildmod.precompress(client), 0)
        self.assertEqual((client / "client.wasm.gz").stat().st_mtime_ns, stamped)

        # A rebuilt asset is newer than its variants, and it is compressed again.
        later = asset.stat().st_mtime_ns + 2_000_000_000
        os.utime(asset, ns=(later, later))
        self.assertEqual(buildmod.precompress(client), 1)
        self.assertGreater((client / "client.wasm.gz").stat().st_mtime_ns, stamped)

    def test_an_older_build_swapped_in_is_compressed_again(self):
        # `synqt build` after `synqt build --release`: the debug artifact is copied in with
        # its own, older time, and the release variants beside it are newer than it.
        client = Path(tempfile.mkdtemp())
        asset = client / "client.wasm"
        asset.write_bytes(b"\x00asm release" + b"x" * 5000)
        buildmod.precompress(client)
        debug = b"\x00asm debug" + b"y" * 5000
        earlier = asset.stat().st_mtime_ns - 60_000_000_000
        asset.write_bytes(debug)
        os.utime(asset, ns=(earlier, earlier))
        self.assertEqual(buildmod.precompress(client), 1)
        self.assertEqual(gzip.decompress((client / "client.wasm.gz").read_bytes()), debug)

    def test_assembling_a_bundle_drops_the_variants_of_another_build(self):
        # `synqt dev` assembles without precompressing, so a variant of the last release
        # build would be served in place of the file it no longer matches.
        client = Path(tempfile.mkdtemp())
        asset = client / "client.wasm"
        asset.write_bytes(b"\x00asm release" + b"x" * 5000)
        (client / "index.html").write_text("<!doctype html>" + "x" * 5000)
        buildmod.precompress(client)
        earlier = asset.stat().st_mtime_ns - 60_000_000_000
        asset.write_bytes(b"\x00asm debug")
        os.utime(asset, ns=(earlier, earlier))
        self.assertEqual(buildmod.drop_stale_variants(client), 2)
        self.assertFalse((client / "client.wasm.gz").exists())
        self.assertTrue((client / "index.html.gz").exists())
        # A variant whose file is gone goes with it.
        (client / "index.html").unlink()
        self.assertEqual(buildmod.drop_stale_variants(client), 2)

    def test_compressing_twice_does_not_compress_the_compressed(self):
        # A second pass does not compress the variants (no client.wasm.gz.gz).
        client = Path(tempfile.mkdtemp())
        (client / "client.wasm").write_bytes(b"\x00asm" + b"x" * 5000)
        buildmod.precompress(client)
        buildmod.precompress(client)
        self.assertFalse((client / "client.wasm.gz.gz").exists())


class ManifestTest(unittest.TestCase):
    def test_manifest_orders_owners_first_and_only_edge_is_public(self):
        config = {
            "entities": [
                {"name": "client", "type": "client"},
                {"name": "web", "type": "web_edge"},
                {"name": "database", "type": "service"},
            ],
            "connect_points": [
                {"owner": "database", "consumers": ["web"]}],
        }
        build_dir = Path(tempfile.mkdtemp())
        path = buildmod.write_process_manifest(config, build_dir)
        manifest = json.loads(path.read_text())
        self.assertLess(manifest["start_order"].index("database"),
                        manifest["start_order"].index("web"))
        binds = {p["entity"]: p["bind"] for p in manifest["processes"]}
        self.assertEqual(binds["web"], "public")
        self.assertEqual(binds["database"], "loopback")
        self.assertEqual(manifest["client_served_from"], {"client": "build/client/"})

    def test_manifest_names_what_each_entity_really_is(self):
        # Each client's own bundle, the binary as Windows names it, and a monitor whose
        # console is bound off loopback faces the public interface too.
        config = {
            "entities": [
                {"name": "app", "type": "client"},
                {"name": "admin", "type": "client"},
                {"name": "web", "type": "web_edge"},
                {"name": "ops", "type": "monitor", "public": {"host": "0.0.0.0"}},
            ],
            "connect_points": [{"owner": "web", "consumers": ["app", "admin"]}],
        }
        build_dir = Path(tempfile.mkdtemp())
        manifest = json.loads(buildmod.write_process_manifest(
            config, build_dir, platform="windows").read_text())
        self.assertEqual(manifest["client_served_from"],
                         {"app": "build/client-app/", "admin": "build/client-admin/"})
        processes = {p["entity"]: p for p in manifest["processes"]}
        self.assertEqual(processes["web"]["binary"], "build/web/web.exe")
        self.assertEqual(processes["ops"]["bind"], "public")
        config["entities"][3]["public"] = {}
        manifest = json.loads(buildmod.write_process_manifest(
            config, build_dir, platform="linux").read_text())
        processes = {p["entity"]: p for p in manifest["processes"]}
        self.assertEqual(processes["ops"]["bind"], "loopback")
        self.assertEqual(processes["web"]["binary"], "build/web/web")


class AssembleBundleTest(unittest.TestCase):
    def test_wasm_outputs_become_a_csp_clean_served_bundle(self):
        wasm = Path(tempfile.mkdtemp())
        (wasm / "client.html").write_text("<body onload='init()'>")  # Qt's template, dropped
        (wasm / "client.js").write_text("// runtime")
        (wasm / "client.wasm").write_bytes(b"\x00asm")
        (wasm / "qtloader.js").write_text("// loader")
        (wasm / "qtlogo.svg").write_text("<svg/>")  # Qt's mark, dropped with its template
        client = Path(tempfile.mkdtemp())
        count = buildmod.assemble_bundle(wasm, client, {}, wasm)
        # Three runtime files copied + index.html + synqt-boot.js + synqt-sw.js + manifest.
        self.assertEqual(count, 7)
        for name in ("client.js", "client.wasm", "qtloader.js",
                     "index.html", "synqt-boot.js", "synqt-sw.js", "synqt-manifest.json"):
            self.assertTrue((client / name).exists(), name)
        # The worker must be in the manifest it precaches from, or it never caches itself.
        listed = json.loads((client / "synqt-manifest.json").read_text())["files"]
        self.assertIn("synqt-sw.js", listed)
        # Qt's inline-handler template is not served. SynQt's shell has no inline handler.
        self.assertFalse((client / "client.html").exists())
        # Nor is Qt's logo: SynQt's shell inlines the SynQt mark, so nothing references it.
        self.assertFalse((client / "qtlogo.svg").exists())
        index = (client / "index.html").read_text()
        self.assertNotIn("onload=", index)
        self.assertIn('src="synqt-boot.js"', index)
        self.assertIn('src="client.js"', index)
        # The document carries the loading background too, so no white flashes between the
        # overlay and the first QML paint.
        self.assertIn("html, body {", index)
        self.assertEqual(index.count(loadingpage.DEFAULT_BACKGROUND), 2)
        # The boot script calls the target's entry symbol.
        self.assertIn("window.client_entry", (client / "synqt-boot.js").read_text())


class BuildLayoutTest(unittest.TestCase):
    def test_build_emits_manifest_and_desktop_layout(self):
        import yaml
        parent = Path(tempfile.mkdtemp())
        newproject.scaffold(parent, "app")
        root = parent / "app"
        # Declare a desktop target so build lays out client-desktop/.
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        config["entities"][0]["targets"] = ["wasm", "desktop"]
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

        # Desktop (host kit) only; AssembleBundleTest covers the browser client.
        buildmod.build(root, profile_name="release", client="desktop")
        # The host's own folder: a desktop build lands under the platform it was built on.
        platform_dir = root / "build" / "client-desktop" / buildmod.desktop_platform()
        self.assertTrue((root / "build" / "process-manifest.json").exists())
        self.assertTrue((platform_dir / "THIRD-PARTY-LICENSES").exists())
        self.assertTrue((root / "build" / "client-desktop" / "DEPLOY.txt").exists())
        # The desktop client is LGPLv3 (no WASM platform port).
        self.assertIn("LGPL-3.0-only", (platform_dir / "THIRD-PARTY-LICENSES").read_text())

    def test_desktop_platform_names_the_host_folder_docs_promise(self):
        # docs/desktop.md names these three folders. Asserted per platform, patched on
        # toolchain, where the host name is decided.
        for platform, expected in [("win32", "windows"), ("darwin", "macos"),
                                   ("linux", "linux"), ("freebsd14", "linux")]:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    self.assertEqual(buildmod.desktop_platform(), expected)

    def test_desktop_edge_url_extracted_for_baking(self):
        # build.desktop.edge_url reaches the compile as SYNQT_EDGE_URL through
        # _desktop_edge_url (tests/desktop-client guards the compile). Absent or blank keeps
        # the default.
        self.assertEqual(
            buildmod._desktop_edge_url(
                {"build": {"desktop": {"edge_url": "wss://edge.example:9443/sync"}}}),
            "wss://edge.example:9443/sync")
        self.assertIsNone(buildmod._desktop_edge_url({"build": {"desktop": {"edge_url": ""}}}))
        self.assertIsNone(buildmod._desktop_edge_url({"build": {"client_threads": "single"}}))
        self.assertIsNone(buildmod._desktop_edge_url({}))


class AppGenTest(unittest.TestCase):
    def test_scaffold_emits_buildable_cmake_and_mains(self):
        parent = Path(tempfile.mkdtemp())
        newproject.scaffold(parent, "app")
        root = parent / "app"
        cmake = (root / "generated" / "synqt.cmake").read_text()
        # The client is always a target; services are guarded by the WASM check.
        self.assertIn("qt_add_executable(app", cmake)
        self.assertIn("if(NOT EMSCRIPTEN)", cmake)
        self.assertIn("qt_add_executable(edge", cmake)
        self.assertIn("SYNQT_ROOT", cmake)
        # The absolute-path QML needs a resource alias, or Qt refuses to configure.
        self.assertIn("QT_RESOURCE_ALIAS", cmake)
        # Each entity gets a main.cpp of the right shape.
        client_main = (root / "generated" / "client" / "app" / "main.cpp").read_text()
        self.assertIn("SynClient", client_main)
        self.assertIn("resolveEdgeUrl", client_main)
        edge_main = (root / "generated" / "web" / "edge" / "main.cpp").read_text()
        self.assertIn("WebEdge edge", edge_main)

    def test_connect_point_drives_source_and_replica_wiring(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
            ],
            "connect_points": [
                {"owner": "web", "consumers": ["client"],
                 "export": "prop int value\n"}],
        }
        cmake = cmakegen.render_root_cmakelists(config, "/opt/synqt")
        # The consumer (client) generates the typed Replica. The owner (edge) the Source.
        self.assertIn("synqt_add_contract(client ROLE replica", cmake)
        self.assertIn("synqt_add_contract(web ROLE source", cmake)
        client_main = maingen.render_client_main(config, appmodel.qml_uri(config["project"]["name"]))
        self.assertIn("synqtRegisterWebReplicas();", client_main)
        # The client registers the consumer surface: `Server` is the facade and
        # `Web.on<Signal>` resolves.
        self.assertIn("synqtRegisterWebConsumers();", client_main)
        # An application includes the framework's own compile rules file.
        self.assertIn('include("${SYNQT_ROOT}/cmake/SynQtBuildFlags.cmake")', cmake)
        self.assertNotIn("CMAKE_CXX_STANDARD", cmake)
        edge_main = maingen.render_edge_main(config, config["entities"][1])
        self.assertIn("synqtRegisterWebSources();", edge_main)
        self.assertIn("WebEdgeConnectPoint pointWeb;", edge_main)
        self.assertIn('pointWeb.contract = QStringLiteral("Web");', edge_main)

    def test_client_main_defaults_logging_by_build_type(self):
        # build.client_logging unset: Console in debug, Silent in release.
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "entities": [{"name": "client", "type": "client", "targets": ["wasm"]}],
        }
        client_main = maingen.render_client_main(config, appmodel.qml_uri(config["project"]["name"]))
        self.assertIn('#include "clientlogging.h"', client_main)
        self.assertIn("#ifdef QT_NO_DEBUG", client_main)
        self.assertIn("ClientLogging::install(ClientLogging::Mode::Silent);", client_main)
        self.assertIn("ClientLogging::install(ClientLogging::Mode::Console);", client_main)

    def test_client_main_honors_explicit_logging_mode(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "build": {"client_logging": "none"},
            "entities": [{"name": "client", "type": "client", "targets": ["wasm"]}],
        }
        client_main = maingen.render_client_main(config, appmodel.qml_uri(config["project"]["name"]))
        self.assertIn(
            'ClientLogging::install(ClientLogging::modeFromName(QStringLiteral("none")));',
            client_main)
        self.assertNotIn("#ifdef QT_NO_DEBUG", client_main)

    def test_client_main_normalizes_the_router_fallback(self):
        # A fallback spelled "/c//" is written as "/c": RoutePattern::matches() tolerates
        # only one trailing slash.
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "router": {"fallback": "/c//"},
            "entities": [{"name": "client", "type": "client", "targets": ["wasm"]}],
        }
        client_main = maingen.render_client_main(config, appmodel.qml_uri(config["project"]["name"]))
        self.assertIn('config.routerFallback = QStringLiteral("/c");', client_main)
        self.assertNotIn('QStringLiteral("/c//")', client_main)

    def test_mains_emit_scopes_hierarchical(self):
        # scopes.hierarchical reaches both mains; the edge is the authoritative check.
        # Default true.
        base = {
            "project": {"name": "gate", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user", "moderator"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
            ],
        }
        uri = appmodel.qml_uri(base["project"]["name"])
        client_default = maingen.render_client_main(base, uri)
        edge_default = maingen.render_edge_main(base, base["entities"][1])
        self.assertIn("config.scopesHierarchical = true;", client_default)
        self.assertIn("config.scopesHierarchical = true;", edge_default)

        setbased = {**base, "scopes": {"order": base["scopes"]["order"], "hierarchical": False}}
        client_set = maingen.render_client_main(setbased, uri)
        edge_set = maingen.render_edge_main(setbased, setbased["entities"][1])
        self.assertIn("config.scopesHierarchical = false;", client_set)
        self.assertIn("config.scopesHierarchical = false;", edge_set)

    def test_edge_hosts_its_points_whatever_the_client_is_called(self):
        """A browser-facing point is one a client entity consumes, whatever the client is called."""
        config = {
            "project": {"name": "shop"},
            "entities": [
                {"name": "app", "type": "client"},
                {"name": "edge", "type": "web_edge"},
            ],
            "connect_points": [
                {"owner": "edge", "consumers": ["app"],
                 "export": "prop int highBid\n"},
            ],
        }
        main = maingen.render_edge_main(config, config["entities"][1])
        self.assertIn('pointEdge.name = QStringLiteral("edge")', main)
        self.assertNotIn("No client-facing connect points yet", main)

    def test_edge_main_composes_entity_runtime_for_its_mesh_side(self):
        # An edge consuming a mesh point uses an EntityRuntime and injects each accessor
        # into its Sources' QML context by name.
        config = {
            "project": {"name": "gavel", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
                {"name": "database", "type": "relational"},
            ],
            "connect_points": [
                {"owner": "web", "consumers": ["client"]},
                {"owner": "database",
                 "consumers": ["web"]}],
        }
        edge_main = maingen.render_edge_main(config, config["entities"][1])
        self.assertIn('#include "entityruntime.h"', edge_main)
        self.assertIn('#include "database_consumer.h"', edge_main)
        self.assertIn("synqtRegisterDatabaseConsumers();", edge_main)
        self.assertIn("EntityRuntime runtime{topologyFromJson(topologyJson), &engine};",
                      edge_main)
        self.assertIn(
            'edge.setContextObject(EntityRuntime::accessorName(QStringLiteral("database")),',
            edge_main)
        # accessor() returns the connect point facade as a QObject*.
        self.assertNotIn("#include <QQmlPropertyMap>", edge_main)
        # It still owns and hosts its browser-facing side through WebEdge.
        self.assertIn("synqtRegisterWebSources();", edge_main)
        self.assertIn("WebEdgeConnectPoint pointWeb;", edge_main)

    def test_service_main_includes_qjsonobject_for_the_topology(self):
        # QJsonDocument only forward-declares QJsonObject, so the service main includes it.
        config = {
            "project": {"name": "gavel", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "database", "type": "relational"},
            ],
            "connect_points": [
                {"owner": "database",
                 "consumers": ["web"]}],
        }
        service_main = maingen.render_service_main(config, config["entities"][0])
        self.assertIn("const QJsonObject topologyJson{", service_main)
        self.assertIn("#include <QJsonObject>", service_main)

    def test_root_cmake_guards_the_providers_subdirectory(self):
        # SynQtService links SynQtProviders PUBLIC, so the root CMake guards its
        # add_subdirectory on the target.
        config = {
            "project": {"name": "gavel", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
                {"name": "database", "type": "relational"},
            ],
            "connect_points": [],
        }
        cmake = cmakegen.render_root_cmakelists(config, "/opt/synqt")
        self.assertIn("if(NOT TARGET SynQtProviders)", cmake)
        # Exactly one add_subdirectory of the providers tree (the guarded one).
        self.assertEqual(cmake.count('src/providers" "${CMAKE_BINARY_DIR}/SynQtProviders"'), 1)

    def test_a_topology_with_no_edge_never_reaches_a_gpl_only_module(self):
        """A project of pure services never adds src/edge, never finds HttpServer or
        NetworkAuth (GPLv3-only), and links every entity against SynQtService alone.
        """
        config = {
            "project": {"name": "batch", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous"]},
            "entities": [
                {"name": "database", "type": "relational"},
                {"name": "rollups", "type": "jobs"},
            ],
            "connect_points": [
                {"owner": "database",
                 "consumers": ["rollups"]}],
        }
        cmake = cmakegen.render_root_cmakelists(config, "/opt/synqt")
        self.assertNotIn("src/edge", cmake)
        self.assertNotIn("src/identity", cmake)
        self.assertNotIn("HttpServer", cmake)
        self.assertNotIn("NetworkAuth", cmake)
        self.assertIn('src/service" "${CMAKE_BINARY_DIR}/SynQtService"', cmake)

    def test_each_entity_links_the_library_its_own_licensing_says_it_does(self):
        """The edge takes SynQtEdge, a promoted auth entity SynQtIdentity, the rest neither."""
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
                {"name": "auth", "type": "service"},
                {"name": "database", "type": "relational"},
            ],
            "connect_points": [],
            "identity": {"provider_entity": "auth", "providers": [{"name": "github"}]},
        }
        cmake = cmakegen.render_root_cmakelists(config, "/opt/synqt")
        for entity, library in (("web", "SynQtEdge"), ("auth", "SynQtIdentity"),
                                ("database", "SynQtService")):
            with self.subTest(entity):
                block = cmake.split(f"target_link_libraries({entity} PRIVATE")[1]
                self.assertIn(library, block.split(")")[0])

    def test_edge_main_without_a_mesh_side_stays_minimal(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
            ],
            "connect_points": [
                {"owner": "web", "consumers": ["client"],
                 "export": "prop int value\n"}],
        }
        edge_main = maingen.render_edge_main(config, config["entities"][1])
        self.assertNotIn("EntityRuntime", edge_main)
        self.assertNotIn("entityruntime.h", edge_main)
        self.assertNotIn("setContextObject", edge_main)
        self.assertNotIn("topologyOption", edge_main)

    def test_qml_uri_is_derived_from_project_name(self):
        self.assertEqual(appmodel.qml_uri("my-todo"), "MyTodo")
        self.assertEqual(appmodel.qml_uri("app"), "App")
        self.assertEqual(appmodel.qml_uri(""), "App")

    def test_entity_singletons_are_auto_registered(self):
        # A pragma Shared QML beside the Sources (the arena World) is registered as a
        # singleton type in the entity main.
        root = Path(tempfile.mkdtemp())
        (root / "web" / "web").mkdir(parents=True)
        (root / "web" / "web" / "World.qml").write_text(
            "pragma Singleton\nimport QtQuick\nItem {}\n")
        (root / "web" / "web" / "Arena.qml").write_text("import QtQuick\nItem {}\n")
        self.assertEqual(appmodel.discover_singletons(root / "web" / "web"), ["World"])
        # A plain (non-singleton) Source is not registered as a singleton.
        self.assertEqual(appmodel.discover_singletons(root / "missing"), [])

        config = {
            "project": {"name": "arena", "qt_version": "6.12.0"},
            "scopes": {"order": ["anonymous", "player"]},
            "entities": [
                {"name": "client", "type": "client", "targets": ["wasm"]},
                {"name": "web", "type": "web_edge"},
            ],
            "connect_points": [
                {"owner": "web",
                 "consumers": ["client"], "instance": "caller"}],
        }
        edge_main = maingen.render_edge_main(config, config["entities"][1], ["World"])
        self.assertIn("qmlRegisterSingletonType", edge_main)
        self.assertIn('QStringLiteral("/web/web/World.qml")', edge_main)
        self.assertIn('"SynQt", 1, 0, "World"', edge_main)
        self.assertIn("#include <QUrl>", edge_main)
        # A service with a singleton gains --qml-dir and registers it; one without stays
        # minimal.
        svc = {"name": "sim", "type": "service"}
        with_singleton = maingen.render_service_main(config, svc, ["World"])
        self.assertIn("qmlRegisterSingletonType", with_singleton)
        self.assertIn("qml-dir", with_singleton)
        without = maingen.render_service_main(config, svc, [])
        self.assertNotIn("qmlRegisterSingletonType", without)
        self.assertNotIn("qml-dir", without)


class DevReloadHarnessTest(unittest.TestCase):
    def test_reload_script_is_csp_clean_and_polls_the_token(self):
        script = clientshell.render_dev_reload_js()
        # CSP-clean. External, no eval/inline. Reloads on a token change it fetches.
        self.assertNotIn("eval(", script)
        self.assertIn('fetch("synqt-reload.txt"', script)
        self.assertIn("window.location.reload()", script)

    def test_every_generated_script_opens_in_strict_mode(self):
        # A "use strict" after the first statement is an ordinary string, not a directive.
        for script in (clientshell.render_dev_reload_js(),
                       clientshell.render_boot_js("client", {})):
            body = script[script.index("(function () {") + len("(function () {"):]
            self.assertTrue(body.lstrip().startswith('"use strict";'), body[:80])

    def test_harness_injects_once_and_bumps_the_token(self):
        client = Path(tempfile.mkdtemp())
        (client / "index.html").write_text(
            clientshell.render_client_shell("client.js", {}, client))
        runmod._write_dev_reload_harness(client)
        index = (client / "index.html").read_text()
        self.assertTrue((client / "synqt-dev.js").exists())
        self.assertTrue((client / "synqt-reload.txt").exists())
        self.assertIn('<script src="synqt-dev.js"></script>', index)
        first = (client / "synqt-reload.txt").read_text()

        # Re-running (a rebuild) must not duplicate the script tag but must bump the token.
        runmod._write_dev_reload_harness(client)
        reindexed = (client / "index.html").read_text()
        self.assertEqual(reindexed.count('src="synqt-dev.js"'), 1)
        self.assertNotEqual((client / "synqt-reload.txt").read_text(), first)

    def test_harness_is_a_no_op_when_no_bundle_exists(self):
        # `synqt dev` may run before a client bundle is built. This must not raise.
        runmod._write_dev_reload_harness(Path(tempfile.mkdtemp()) / "client")


class DeployedBinaryTest(unittest.TestCase):
    def test_the_deployed_binary_is_found_whatever_suffix_the_host_adds(self):
        # `synqt serve` resolves the binary suffix (Windows adds .exe).
        root = Path(tempfile.mkdtemp())
        (root / "build" / "web").mkdir(parents=True)
        (root / "build" / "web" / "web.exe").write_text("MZ")  # what Windows leaves on disk
        found = runmod._deployed_binary(root, "web")
        self.assertIsNotNone(found)
        self.assertEqual(found.name, "web.exe")

        (root / "build" / "db").mkdir(parents=True)
        (root / "build" / "db" / "db").write_text("\x7fELF")
        self.assertEqual(runmod._deployed_binary(root, "db").name, "db")
        self.assertIsNone(runmod._deployed_binary(root, "never-built"))


class LaunchEnvTest(unittest.TestCase):
    """What an entity binary is launched with. On Windows the kit bin directory goes on PATH
    (there is no RPATH).
    """

    def test_a_windows_launch_carries_the_qt_kit_bin_on_path(self):
        old = os.environ.get("PATH", "")
        with unittest.mock.patch.object(toolchain, "host_platform", return_value="windows"), \
             unittest.mock.patch.object(runmod, "resolved_host_qt",
                                        return_value=r"C:\Qt\6.12.0\msvc2022_64"):
            env = runmod.launch_env(Path("/proj"))
        # Asserted without the host's path separator.
        prepended = env["PATH"][:-(len(old) + len(os.pathsep))]
        self.assertIn("msvc2022_64", prepended)
        self.assertTrue(prepended.endswith("bin"), prepended)
        self.assertTrue(env["PATH"].endswith(old))  # the inherited PATH survives

    def test_a_windows_launch_without_a_resolved_kit_leaves_path_alone(self):
        with unittest.mock.patch.object(toolchain, "host_platform", return_value="windows"), \
             unittest.mock.patch.object(runmod, "resolved_host_qt", return_value=None):
            env = runmod.launch_env(Path("/proj"))
        self.assertEqual(env["PATH"], os.environ.get("PATH", ""))

    def test_the_other_hosts_are_left_as_they_are_but_headless(self):
        without = {k: v for k, v in os.environ.items() if k != "QT_QPA_PLATFORM"}
        for host in ("linux", "macos"):
            with self.subTest(host=host):
                with unittest.mock.patch.object(toolchain, "host_platform", return_value=host), \
                     unittest.mock.patch.dict(os.environ, without, clear=True):
                    self.assertEqual(runmod.launch_env(Path("/proj")),
                                     {**without, "QT_QPA_PLATFORM": "offscreen"})

    def test_an_entity_needs_no_display_on_any_host(self):
        # An edge runs a QGuiApplication; with no display Qt aborts unless offscreen is set.
        without = {k: v for k, v in os.environ.items() if k != "QT_QPA_PLATFORM"}
        for host in ("linux", "macos", "windows"):
            with self.subTest(host=host):
                with unittest.mock.patch.object(toolchain, "host_platform", return_value=host), \
                     unittest.mock.patch.object(runmod, "resolved_host_qt", return_value=None), \
                     unittest.mock.patch.dict(os.environ, without, clear=True):
                    self.assertEqual(runmod.launch_env(Path("/proj"))["QT_QPA_PLATFORM"],
                                     "offscreen")

    def test_a_platform_the_developer_chose_is_kept(self):
        with unittest.mock.patch.dict(os.environ, {"QT_QPA_PLATFORM": "xcb"}), \
             unittest.mock.patch.object(toolchain, "host_platform", return_value="linux"):
            self.assertEqual(runmod.launch_env(Path("/proj"))["QT_QPA_PLATFORM"], "xcb")


class HotReloadTest(unittest.TestCase):
    def test_a_failed_rebuild_reports_and_keeps_the_dev_server_up(self):
        # `synqt build` raises on a failed compile; the watcher reports it and keeps
        # running. Both use _cmake_build.
        state = {"config": {"entities": []}, "processes": []}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=buildmod.BuildError("cmake build failed: no matching function")):
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        self.assertEqual(state["processes"], [])  # nothing was torn down

    def test_a_config_the_generator_refuses_keeps_the_dev_server_up(self):
        # compile_incremental regenerates first, so a half-written route raises AppGenError,
        # which the watcher reports. A non-yaml change (Main.qml) reaches the rebuild path.
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=appmodel.AppGenError(
                    "route '/admin' declares no view; there is nothing for the router "
                    "to show there")) as compile_mock:
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        compile_mock.assert_called_once()  # the rebuild path ran
        self.assertEqual(len(state["processes"]), 1)  # nothing was torn down

    def test_a_structurally_wrong_config_keeps_the_dev_server_up(self):
        # Valid YAML with a scalar where a mapping belongs (`router: /home`) raises
        # AttributeError in appgen; the watcher's broad catch reports it.
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=AttributeError(
                    "'str' object has no attribute 'get'")) as compile_mock:
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        compile_mock.assert_called_once()  # the rebuild path ran
        self.assertEqual(len(state["processes"]), 1)  # nothing was torn down

    def test_a_synqt_yaml_that_does_not_parse_keeps_the_dev_server_up(self):
        # A YAML error while re-reading the topology is reported the same way.
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text("entities: [\n")
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(buildmod, "compile_incremental") as compile_mock:
            runmod._hot_reload(root, state, 8080, "wasm", {root / "synqt.yaml"})
        compile_mock.assert_not_called()  # never rebuilt against a topology that is gone
        self.assertEqual(len(state["processes"]), 1)


class SourceWatcherTest(unittest.TestCase):
    def test_detects_edited_qml_and_a_topology_change_but_ignores_build(self):
        root = Path(tempfile.mkdtemp())
        (root / "client").mkdir()
        qml = root / "client" / "Main.qml"
        qml.write_text("import QtQuick\nItem {}\n")
        (root / "build" / "wasm").mkdir(parents=True)
        watcher = runmod.SourceWatcher(root)
        self.assertEqual(watcher.poll(), set())  # nothing changed yet

        # A rebuild writing under build/ is ignored.
        (root / "build" / "wasm" / "client.js").write_text("// runtime")
        self.assertEqual(watcher.poll(), set())

        # A QML edit and a configuration change are seen. No `.syn` is watched: contracts
        # are generated under generated/.
        import os as _os
        _os.utime(qml, ns=(2 ** 40, 2 ** 40))
        (root / "synqt.yaml").write_text("entities: []\n")
        changed = watcher.poll()
        self.assertIn(qml, changed)
        self.assertIn(root / "synqt.yaml", changed)

    def test_categorize_routes_changes_to_the_right_side(self):
        root = Path("/proj")
        config = {"entities": [
            {"name": "client", "type": "client"},
            {"name": "web", "type": "web_edge"},
            {"name": "database", "type": "service"}]}
        # A client QML edit rebuilds only the client.
        self.assertEqual(
            runmod._categorize({root / "client" / "client" / "Main.qml"}, root, config),
            (False, True))
        # A service QML edit rebuilds only the host side.
        self.assertEqual(
            runmod._categorize({root / "service" / "database" / "Items.qml"}, root, config),
            (True, False))
        # A topology change is a change to what crosses every link, so it rebuilds both.
        self.assertEqual(runmod._categorize({root / "synqt.yaml"}, root, config), (True, True))


if __name__ == "__main__":
    unittest.main()


class UserPresetTest(unittest.TestCase):
    def test_the_user_preset_stub_sets_nothing_the_build_would_not_read(self):
        # A cache variable no CMake file reads makes every configure through the preset warn
        # that it was not used.
        root = Path(tempfile.mkdtemp())
        from synqt import presets
        presets.write(root, {"entities": [{"name": "app", "type": "client"}]})
        user = json.loads((root / "CMakeUserPresets.json").read_text())
        for preset in user["configurePresets"]:
            self.assertNotIn("cacheVariables", preset)
