# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""M10 toolchain: resolution, .wasm precompression, the process manifest, desktop layout."""

import gzip
import json
import os
import shutil
import tempfile
import unittest
import unittest.mock
from pathlib import Path

from synqt import appgen
from synqt import loadingpage
from synqt import build as buildmod
from synqt import newproject, toolchain
from synqt import run as runmod


class ToolchainTest(unittest.TestCase):
    def test_resolves_installed_kits_or_reports_hints(self):
        resolved = toolchain.resolve(tempfile.mkdtemp())
        self.assertEqual(resolved["qt_version"], "6.11.1")
        self.assertEqual(resolved["emscripten_version"], "4.0.7")
        # On a host with a system Qt these resolve; otherwise the report gives the exact
        # aqtinstall/emsdk command. Either way the shape is stable.
        if not resolved["host_qt"]:
            self.assertTrue(any("aqt install-qt" in h for h in toolchain.provision_hints(resolved)))
        self.assertIn("Toolchain (Qt 6.11.1", toolchain.report(tempfile.mkdtemp()))

    def test_provision_hints_carry_the_coordinates_aqt_actually_accepts(self):
        # A developer copies these out of `synqt doctor` and runs them, so a hint that does not
        # parse is a broken instruction, not cosmetic. Both were wrong: the host arch was named
        # by its install directory (gcc_64) rather than its arch (linux_gcc_64), and the WASM kit
        # was requested under the desktop target, which fails with "Failed to locate XML data for
        # Qt version": an error about the version, for a mistake in the coordinates.
        #
        # The host hint is per platform: it named the Linux kit on every host, so `synqt
        # doctor` on a Mac printed a command that installs the wrong Qt. The WASM hint is
        # host-independent by design and must stay all_os/wasm everywhere.
        cases = [("win32", "install-qt windows desktop 6.11.1 win64_msvc2022_64"),
                 ("darwin", "install-qt mac desktop 6.11.1 clang_64"),
                 ("linux", "install-qt linux desktop 6.11.1 linux_gcc_64")]
        for platform, expected in cases:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    hints = toolchain.provision_hints({"wasm_kit": "wasm_singlethread",
                                                       "wasm_qt": None, "host_qt": None,
                                                       "emcc": None})
                host = next(h for h in hints if "desktop" in h)
                wasm = next(h for h in hints if "wasm_singlethread" in h)
                self.assertIn(expected, host)
                self.assertIn("install-qt all_os wasm 6.11.1 wasm_singlethread", wasm)

    def test_host_kit_directory_is_the_hosts_own_never_a_hard_coded_linux_one(self):
        # The kit directory Qt installs into differs per platform. It was hard-coded to
        # gcc_64, so on macOS and Windows the resolver looked for a Linux kit, never found
        # one, and every `synqt build` reported "toolchain incomplete" and skipped the
        # compile: reporting a missing toolchain on a machine where Qt was installed.
        for platform, expected in [("win32", "msvc2022_64"), ("darwin", "macos"),
                                   ("linux", "gcc_64"), ("freebsd14", "gcc_64")]:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    self.assertEqual(toolchain.host_kit_dir(), expected)

    def test_an_explicit_qtdir_wins_and_never_stands_in_for_the_wasm_kit(self):
        root = Path(tempfile.mkdtemp())
        qt = root / "Qt" / "6.11.1"
        (qt / "gcc_64" / "lib" / "cmake").mkdir(parents=True)
        (qt / "wasm_singlethread" / "lib" / "cmake").mkdir(parents=True)
        project = root / "project"
        project.mkdir()

        with unittest.mock.patch.object(toolchain.sys, "platform", "linux"), \
                unittest.mock.patch.dict(os.environ, {"QTDIR": str(qt / "gcc_64")}):
            resolved = toolchain.resolve(project)
            # QTDIR is an explicit choice, so it beats whatever system Qt happens to exist
            # on the machine running this test.
            self.assertEqual(resolved["host_qt"], str(qt / "gcc_64"))
            # The WASM kit is found as QTDIR's sibling, which is how one kit dir locates
            # the other.
            self.assertEqual(resolved["wasm_qt"], str(qt / "wasm_singlethread"))

            # ...but QTDIR must never be handed back AS the WASM kit. Accepting it for any
            # kit asked for would silently build the browser client against the host kit.
            shutil.rmtree(qt / "wasm_singlethread")
            self.assertNotEqual(toolchain.resolve(project)["wasm_qt"], str(qt / "gcc_64"))


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
        # The Emscripten glue .js is the second-largest asset on a first visit, so
        # compressing only the wasm leaves real bytes on the table.
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

    def test_compressing_twice_does_not_compress_the_compressed(self):
        # The variants live beside their source, so a second pass must not pick them up
        # and produce client.wasm.gz.gz.
        client = Path(tempfile.mkdtemp())
        (client / "client.wasm").write_bytes(b"\x00asm" + b"x" * 5000)
        buildmod.precompress(client)
        buildmod.precompress(client)
        self.assertFalse((client / "client.wasm.gz.gz").exists())


class ManifestTest(unittest.TestCase):
    def test_manifest_orders_owners_first_and_only_edge_is_public(self):
        config = {
            "entities": [
                {"name": "client", "kind": "client"},
                {"name": "web", "kind": "service", "capability": "web_edge"},
                {"name": "database", "kind": "service"},
            ],
            "connect_points": [
                {"name": "items", "owner": "database", "consumers": ["web"]}],
        }
        build_dir = Path(tempfile.mkdtemp())
        path = buildmod.write_process_manifest(config, build_dir)
        manifest = json.loads(path.read_text())
        self.assertLess(manifest["start_order"].index("database"),
                        manifest["start_order"].index("web"))
        binds = {p["entity"]: p["bind"] for p in manifest["processes"]}
        self.assertEqual(binds["web"], "public")
        self.assertEqual(binds["database"], "loopback")


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
        # Qt's inline-handler template is not served; our shell has no inline handler.
        self.assertFalse((client / "client.html").exists())
        # Nor is Qt's logo: SynQt's shell inlines the SynQt mark, so nothing references it.
        self.assertFalse((client / "qtlogo.svg").exists())
        index = (client / "index.html").read_text()
        self.assertNotIn("onload=", index)
        self.assertIn('src="synqt-boot.js"', index)
        self.assertIn('src="client.js"', index)
        # The document itself carries the loading background, not only the overlay: the
        # overlay is hidden a frame or two before the first QML paint, and the browser's
        # default white would flash through that gap.
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

        # Desktop (host kit) only; the browser client compiles through the separate wasm
        # kit and is covered compile-free by AssembleBundleTest.
        buildmod.build(root, release=True, client="desktop")
        # The host's own folder, not a hard-coded "linux": a desktop build is native, so it lands
        # under the platform it was built on. This test previously asserted linux/ and so could
        # only ever have passed on Linux.
        platform_dir = root / "build" / "client-desktop" / buildmod.desktop_platform()
        self.assertTrue((root / "build" / "process-manifest.json").exists())
        self.assertTrue((platform_dir / "THIRD-PARTY-LICENSES").exists())
        self.assertTrue((root / "build" / "client-desktop" / "DEPLOY.txt").exists())
        # The desktop client is LGPLv3 (no WASM platform port).
        self.assertIn("LGPL-3.0-only", (platform_dir / "THIRD-PARTY-LICENSES").read_text())

    def test_desktop_platform_names_the_host_folder_docs_promise(self):
        # docs/desktop.md names exactly these three folders. Asserted per platform rather than
        # against the current host, because the bug this pins was invisible on Linux: the folder
        # was hard-coded to linux/, so a Windows or macOS desktop build wrote its app into a
        # directory named after someone else's operating system.
        #
        # Patched on toolchain, which is where the host name is now decided: build and the
        # toolchain resolver each had their own copy of this, and the second copy is what
        # went stale.
        for platform, expected in [("win32", "windows"), ("darwin", "macos"),
                                   ("linux", "linux"), ("freebsd14", "linux")]:
            with self.subTest(sys_platform=platform):
                with unittest.mock.patch.object(toolchain.sys, "platform", platform):
                    self.assertEqual(buildmod.desktop_platform(), expected)

    def test_desktop_edge_url_extracted_for_baking(self):
        # A native desktop client has no serving origin, so build.desktop.edge_url must reach the
        # compile as SYNQT_EDGE_URL. _desktop_edge_url is the seam that pulls it out of the config
        # (the compile-level guard is tests/desktop-client). Absent/blank means keep the default.
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
        cmake = (root / "CMakeLists.txt").read_text()
        # The client is always a target; services are guarded behind the WASM check so a
        # WebAssembly configure builds only the client.
        self.assertIn("qt_add_executable(client", cmake)
        self.assertIn("if(NOT EMSCRIPTEN)", cmake)
        self.assertIn("qt_add_executable(web", cmake)
        self.assertIn("SYNQT_ROOT", cmake)
        # The absolute-path QML needs a resource alias, or Qt refuses to configure.
        self.assertIn("QT_RESOURCE_ALIAS", cmake)
        # Each entity gets a main.cpp of the right shape.
        client_main = (root / "client" / "main.cpp").read_text()
        self.assertIn("SynClient", client_main)
        self.assertIn("resolveEdgeUrl", client_main)
        edge_main = (root / "web" / "main.cpp").read_text()
        self.assertIn("WebEdge edge", edge_main)

    def test_connect_point_drives_source_and_replica_wiring(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "kind": "client", "targets": ["wasm"]},
                {"name": "web", "kind": "service", "capability": "web_edge"},
            ],
            "connect_points": [
                {"name": "counter", "contract": "Counter", "owner": "web",
                 "consumers": ["client"], "instance": "shared"}],
        }
        cmake = appgen.render_root_cmakelists(config, "/opt/synqt")
        # The consumer (client) generates the typed Replica; the owner (edge) the Source.
        self.assertIn("synqt_add_contract(client ROLE replica", cmake)
        self.assertIn("synqt_add_contract(web ROLE source", cmake)
        client_main = appgen.render_client_main(config, appgen.qml_uri(config["project"]["name"]))
        self.assertIn("synqtRegisterCounterReplicas();", client_main)
        # The client also registers the consumer surface, so Server.counter is the facade
        # (returning-slot promises) and `Counter.on<Signal>` handlers resolve.
        self.assertIn("synqtRegisterCounterConsumers();", client_main)
        edge_main = appgen.render_edge_main(config, config["entities"][1])
        self.assertIn("synqtRegisterCounterSources();", edge_main)
        self.assertIn("WebEdgeConnectPoint counter;", edge_main)
        self.assertIn('counter.contract = QStringLiteral("Counter");', edge_main)

    def test_client_main_defaults_logging_by_build_type(self):
        # With build.client_logging unset, the generated main installs Console in a debug
        # build and Silent in a release build; so console.log works in dev, stripped in prod.
        config = {
            "project": {"name": "shop", "qt_version": "6.11.1"},
            "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]}],
        }
        client_main = appgen.render_client_main(config, appgen.qml_uri(config["project"]["name"]))
        self.assertIn('#include "clientlogging.h"', client_main)
        self.assertIn("#ifdef QT_NO_DEBUG", client_main)
        self.assertIn("ClientLogging::install(ClientLogging::Mode::Silent);", client_main)
        self.assertIn("ClientLogging::install(ClientLogging::Mode::Console);", client_main)

    def test_client_main_honors_explicit_logging_mode(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.11.1"},
            "build": {"client_logging": "none"},
            "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]}],
        }
        client_main = appgen.render_client_main(config, appgen.qml_uri(config["project"]["name"]))
        self.assertIn(
            'ClientLogging::install(ClientLogging::modeFromName(QStringLiteral("none")));',
            client_main)
        self.assertNotIn("#ifdef QT_NO_DEBUG", client_main)

    def test_client_main_normalizes_the_router_fallback(self):
        # `synqt check` treats "/c" and "/c//" as one route (RoutePattern skips empty
        # segments), so a fallback spelled "/c//" passes. The client, though, looks the
        # fallback up with RoutePattern::matches(), which tolerates only one trailing
        # slash: the raw "/c//" would match nothing and blank the page. The generator
        # writes the fallback through the same collapse rule so the two agree.
        config = {
            "project": {"name": "shop", "qt_version": "6.11.1"},
            "router": {"fallback": "/c//"},
            "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]}],
        }
        client_main = appgen.render_client_main(config, appgen.qml_uri(config["project"]["name"]))
        self.assertIn('config.routerFallback = QStringLiteral("/c");', client_main)
        self.assertNotIn('QStringLiteral("/c//")', client_main)

    def test_edge_main_composes_entity_runtime_for_its_mesh_side(self):
        # A web edge that consumes a database connect point over the mesh reaches it through
        # an EntityRuntime (WebEdge keeps the browser side); each acquired accessor is injected
        # into the owner Sources' QML context by name, so a Source can delegate over the mesh.
        config = {
            "project": {"name": "gavel", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "kind": "client", "targets": ["wasm"]},
                {"name": "web", "kind": "service", "capability": "web_edge"},
                {"name": "database", "kind": "service", "blueprint": "persistence"},
            ],
            "connect_points": [
                {"name": "auction", "contract": "Auction", "owner": "web",
                 "consumers": ["client"], "instance": "per_session"},
                {"name": "ledger", "contract": "Ledger", "owner": "database",
                 "consumers": ["web"]}],
        }
        edge_main = appgen.render_edge_main(config, config["entities"][1])
        self.assertIn('#include "entityruntime.h"', edge_main)
        self.assertIn('#include "ledger_consumer.h"', edge_main)
        self.assertIn("synqtRegisterLedgerConsumers();", edge_main)
        self.assertIn("EntityRuntime runtime{topologyFromJson(topologyJson), &engine};",
                      edge_main)
        self.assertIn(
            'edge.setContextObject(EntityRuntime::accessorName(QStringLiteral("database")),',
            edge_main)
        # accessor() returns a QQmlPropertyMap*, upcast to QObject* for setContextObject, so
        # the full type must be included or the mesh edge does not compile.
        self.assertIn("#include <QQmlPropertyMap>", edge_main)
        # It still owns and hosts its browser-facing side through WebEdge.
        self.assertIn("synqtRegisterAuctionSources();", edge_main)
        self.assertIn("WebEdgeConnectPoint auction;", edge_main)

    def test_service_main_includes_qjsonobject_for_the_topology(self):
        # The service main builds a const QJsonObject from the topology file; QJsonDocument's
        # header only forward-declares QJsonObject, so it must be included in its own right or
        # the entity does not compile.
        config = {
            "project": {"name": "gavel", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "database", "kind": "service", "blueprint": "persistence"},
            ],
            "connect_points": [
                {"name": "ledger", "contract": "Ledger", "owner": "database",
                 "consumers": ["web"]}],
        }
        service_main = appgen.render_service_main(config, config["entities"][0])
        self.assertIn("const QJsonObject topologyJson{", service_main)
        self.assertIn("#include <QJsonObject>", service_main)

    def test_root_cmake_guards_the_providers_subdirectory(self):
        # SynQtService already pulls SynQtProviders in (it PUBLIC-links it), so the root CMake
        # must guard its own add_subdirectory on the target or it claims the same binary
        # directory twice and configuration fails.
        config = {
            "project": {"name": "gavel", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "kind": "client", "targets": ["wasm"]},
                {"name": "web", "kind": "service", "capability": "web_edge"},
                {"name": "database", "kind": "service", "blueprint": "persistence"},
            ],
            "connect_points": [],
        }
        cmake = appgen.render_root_cmakelists(config, "/opt/synqt")
        self.assertIn("if(NOT TARGET SynQtProviders)", cmake)
        # Exactly one add_subdirectory of the providers tree (the guarded one).
        self.assertEqual(cmake.count('src/providers" "${CMAKE_BINARY_DIR}/SynQtProviders"'), 1)

    def test_edge_main_without_a_mesh_side_stays_minimal(self):
        config = {
            "project": {"name": "shop", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "user"]},
            "entities": [
                {"name": "client", "kind": "client", "targets": ["wasm"]},
                {"name": "web", "kind": "service", "capability": "web_edge"},
            ],
            "connect_points": [
                {"name": "counter", "contract": "Counter", "owner": "web",
                 "consumers": ["client"], "instance": "shared"}],
        }
        edge_main = appgen.render_edge_main(config, config["entities"][1])
        self.assertNotIn("EntityRuntime", edge_main)
        self.assertNotIn("entityruntime.h", edge_main)
        self.assertNotIn("setContextObject", edge_main)
        self.assertNotIn("topologyOption", edge_main)

    def test_qml_uri_is_derived_from_project_name(self):
        self.assertEqual(appgen.qml_uri("my-todo"), "MyTodo")
        self.assertEqual(appgen.qml_uri("app"), "App")
        self.assertEqual(appgen.qml_uri(""), "App")

    def test_entity_singletons_are_auto_registered(self):
        # A pragma-Singleton QML alongside an entity's Sources (the arena's World) is not a
        # QML-module member, so the entity's main.cpp must register it as a singleton type
        # for a Source that consumes it (World.steer(...)) to resolve it by name.
        root = Path(tempfile.mkdtemp())
        (root / "web").mkdir()
        (root / "web" / "World.qml").write_text(
            "pragma Singleton\nimport QtQuick\nItem {}\n")
        (root / "web" / "Arena.qml").write_text("import QtQuick\nItem {}\n")
        self.assertEqual(appgen.discover_singletons(root / "web"), ["World"])
        # A plain (non-singleton) Source is not registered as a singleton.
        self.assertEqual(appgen.discover_singletons(root / "missing"), [])

        config = {
            "project": {"name": "arena", "qt_version": "6.11.1"},
            "scopes": {"order": ["anonymous", "player"]},
            "entities": [
                {"name": "client", "kind": "client", "targets": ["wasm"]},
                {"name": "web", "kind": "service", "capability": "web_edge"},
            ],
            "connect_points": [
                {"name": "arena", "contract": "Arena", "owner": "web",
                 "consumers": ["client"], "instance": "per_session"}],
        }
        edge_main = appgen.render_edge_main(config, config["entities"][1], ["World"])
        self.assertIn("qmlRegisterSingletonType", edge_main)
        self.assertIn('QStringLiteral("/web/World.qml")', edge_main)
        self.assertIn('"SynQt", 1, 0, "World"', edge_main)
        self.assertIn("#include <QUrl>", edge_main)
        # A service that declares a singleton gains a --qml-dir and registers it too; a
        # service without one stays minimal (no qml-dir option, no registration).
        svc = {"name": "sim", "kind": "service"}
        with_singleton = appgen.render_service_main(config, svc, ["World"])
        self.assertIn("qmlRegisterSingletonType", with_singleton)
        self.assertIn("qml-dir", with_singleton)
        without = appgen.render_service_main(config, svc, [])
        self.assertNotIn("qmlRegisterSingletonType", without)
        self.assertNotIn("qml-dir", without)


class DevReloadHarnessTest(unittest.TestCase):
    def test_reload_script_is_csp_clean_and_polls_the_token(self):
        script = appgen.render_dev_reload_js()
        # CSP-clean: external, no eval/inline; reloads on a token change it fetches.
        self.assertNotIn("eval(", script)
        self.assertIn('fetch("synqt-reload.txt"', script)
        self.assertIn("window.location.reload()", script)

    def test_harness_injects_once_and_bumps_the_token(self):
        client = Path(tempfile.mkdtemp())
        (client / "index.html").write_text(
            appgen.render_client_shell("client.js", {}, client))
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
        # `synqt dev` may run before a client bundle is built; this must not raise.
        runmod._write_dev_reload_harness(Path(tempfile.mkdtemp()) / "client")


class DeployedBinaryTest(unittest.TestCase):
    def test_the_deployed_binary_is_found_whatever_suffix_the_host_adds(self):
        # `synqt serve` used to name build/<entity>/<entity> directly, so on Windows it found
        # nothing and reported every entity of a perfectly good build as "Not yet built" --
        # the same defect host_binary() already carried a docstring about, in the one place
        # that did not use it.
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
    """What an entity binary is launched with. Windows has no RPATH, so a built entity cannot
    see Qt6Core.dll in the kit's bin and dies before main(), behind an error dialog, which
    on CI reads as a hang rather than a failure. Linux and macOS bake the path in at link
    time, which is why this is invisible on two hosts out of three."""

    def test_a_windows_launch_carries_the_qt_kit_bin_on_path(self):
        old = os.environ.get("PATH", "")
        with unittest.mock.patch.object(toolchain, "host_platform", return_value="windows"), \
             unittest.mock.patch.object(runmod, "resolved_host_qt",
                                        return_value=r"C:\Qt\6.11.1\msvc2022_64"):
            env = runmod.launch_env(Path("/proj"))
        # Asserted os-agnostically: this test runs on a host whose os.pathsep and Path
        # flavour are not Windows's, so splitting on the separator would be testing the
        # harness rather than the code.
        prepended = env["PATH"][:-(len(old) + len(os.pathsep))]
        self.assertIn("msvc2022_64", prepended)
        self.assertTrue(prepended.endswith("bin"), prepended)
        self.assertTrue(env["PATH"].endswith(old))  # the inherited PATH survives

    def test_a_windows_launch_without_a_resolved_kit_leaves_path_alone(self):
        with unittest.mock.patch.object(toolchain, "host_platform", return_value="windows"), \
             unittest.mock.patch.object(runmod, "resolved_host_qt", return_value=None):
            env = runmod.launch_env(Path("/proj"))
        self.assertEqual(env["PATH"], os.environ.get("PATH", ""))

    def test_the_other_hosts_are_left_exactly_as_they_are(self):
        for host in ("linux", "macos"):
            with self.subTest(host=host):
                with unittest.mock.patch.object(toolchain, "host_platform", return_value=host):
                    self.assertEqual(runmod.launch_env(Path("/proj")), dict(os.environ))


class HotReloadTest(unittest.TestCase):
    def test_a_failed_rebuild_reports_and_keeps_the_dev_server_up(self):
        # `synqt build` raises on a failed compile, because it must never claim an artifact it
        # did not produce. The watcher wants the opposite: you fix the typo and save again, so
        # a broken rebuild is news, not the end of the session. Both behaviours come from the
        # same _cmake_build, so this pins the seam between them; without it the two are one
        # edit away from collapsing into each other, and the failure mode (a traceback that
        # kills a running dev server on a syntax error) is a miserable one to rediscover.
        state = {"config": {"entities": []}, "processes": []}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=buildmod.BuildError("cmake build failed: no matching function")):
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        self.assertEqual(state["processes"], [])  # nothing was torn down

    def test_a_config_the_generator_refuses_keeps_the_dev_server_up(self):
        # compile_incremental regenerates before it compiles, and the generator refuses a
        # config it cannot lower (a route saved before its `view` is typed). That is
        # exactly the edit the watcher exists for, so it has to be news like a failed
        # compile; letting AppGenError out of here escapes into _watch_loop, whose finally
        # terminates every child process, and the whole dev system goes down on a
        # half-finished save. The change set is a non-yaml source (Main.qml) so the rebuild
        # path is actually reached, not the load_config early return the sibling test covers.
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=appgen.AppGenError(
                    "route '/admin' declares no view; there is nothing for the router "
                    "to show there")) as compile_mock:
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        compile_mock.assert_called_once()  # the rebuild path really ran
        self.assertEqual(len(state["processes"]), 1)  # nothing was torn down

    def test_a_structurally_wrong_config_keeps_the_dev_server_up(self):
        # A synqt.yaml that parses as valid YAML but puts a scalar where the generator
        # expects a mapping (`router: /home` before its indented `fallback:` is typed)
        # reaches appgen and raises a bare AttributeError, in neither the BuildError nor the
        # AppGenError tuple. It is the same half-finished save the watcher exists for, so it
        # must be news too; the broad fallback catch pins this and stops the tuple from
        # narrowing back and killing a running dev server on a mid-edit save.
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(
                buildmod, "compile_incremental",
                side_effect=AttributeError(
                    "'str' object has no attribute 'get'")) as compile_mock:
            runmod._hot_reload(Path(tempfile.mkdtemp()), state, 8080, "wasm",
                               {Path("Main.qml")})
        compile_mock.assert_called_once()  # the rebuild path really ran
        self.assertEqual(len(state["processes"]), 1)  # nothing was torn down

    def test_a_synqt_yaml_that_does_not_parse_keeps_the_dev_server_up(self):
        # The same half-finished save, one step earlier: the topology is re-read before
        # the rebuild, and a YAML error there would escape the same way.
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text("entities: [\n")
        state = {"config": {"entities": []}, "processes": [("web", object())]}
        with unittest.mock.patch.object(buildmod, "compile_incremental") as compile_mock:
            runmod._hot_reload(root, state, 8080, "wasm", {root / "synqt.yaml"})
        compile_mock.assert_not_called()  # never rebuilt against a topology that is gone
        self.assertEqual(len(state["processes"]), 1)


class SourceWatcherTest(unittest.TestCase):
    def test_detects_edited_qml_and_new_contract_but_ignores_build(self):
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

        # A QML edit and a new .syn are both seen.
        import os as _os
        _os.utime(qml, ns=(2 ** 40, 2 ** 40))
        (root / "shared").mkdir()
        (root / "shared" / "Counter.syn").write_text("contract Counter {}\n")
        changed = watcher.poll()
        self.assertIn(qml, changed)
        self.assertIn(root / "shared" / "Counter.syn", changed)

    def test_categorize_routes_changes_to_the_right_side(self):
        root = Path("/proj")
        config = {"entities": [
            {"name": "client", "kind": "client"},
            {"name": "web", "kind": "service", "capability": "web_edge"},
            {"name": "database", "kind": "service"}]}
        # A client QML edit rebuilds only the client.
        self.assertEqual(
            runmod._categorize({root / "client" / "Main.qml"}, root, config), (False, True))
        # A service QML edit rebuilds only the host side.
        self.assertEqual(
            runmod._categorize({root / "database" / "Items.qml"}, root, config), (True, False))
        # Topology and contract changes rebuild both.
        self.assertEqual(runmod._categorize({root / "synqt.yaml"}, root, config), (True, True))
        self.assertEqual(
            runmod._categorize({root / "shared" / "Items.syn"}, root, config), (True, True))


if __name__ == "__main__":
    unittest.main()
