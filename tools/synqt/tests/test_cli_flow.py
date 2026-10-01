# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The mesh CA tooling, the license generation, and the new/check/build/doctor flow."""

import os
import re
import stat
import subprocess
import tempfile
import unittest
import unittest.mock
from datetime import datetime, timedelta, timezone
from pathlib import Path

import yaml

from synqt import build as buildmod
from synqt import cmakegen
from synqt import appmodel
from synqt import check, config as configmod, doctor, licenses, mesh, newproject, toolchain


class MeshTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())

    def test_ca_key_is_private_and_gitignored_never_a_client_cert(self):
        mesh.init(self.root)
        ca_key = self.root / "synqt" / "mesh" / "ca.key"
        self.assertTrue(ca_key.exists())
        # The CA key is 0600 where POSIX modes apply. The Windows ACL half (mesh.restrict)
        # cannot be asserted from a POSIX host.
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(ca_key.stat().st_mode), 0o600)
        # ...and git-ignored, so it is never committed.
        self.assertIn("synqt/mesh/*.key", (self.root / ".gitignore").read_text())

        # The subject is the entity name. -nameopt RFC2253 gives a format stable across
        # openssl versions.
        mesh.cert(self.root, "database")
        self.assertTrue((self.root / "synqt" / "mesh" / "database.crt").exists())
        import subprocess
        subject = subprocess.check_output(
            ["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253", "-in",
             str(self.root / "synqt" / "mesh" / "database.crt")], text=True)
        self.assertIn("CN=database", subject)

        # The client gets no mesh certificate.
        with self.assertRaises(mesh.MeshError):
            mesh.cert(self.root, "client", kind="client")

    def test_a_private_key_is_never_readable_by_anyone_else_even_briefly(self):
        """The key file is created with its final mode, not chmod-ed afterwards.

        Run with a permissive umask, which makes the window observable; the mode is taken at
        the moment a stand-in for openssl starts writing.
        """
        if os.name == "nt":
            self.skipTest("POSIX modes; mesh.restrict carries the Windows ACL half")
        seen = {}
        real = mesh._openssl

        def watching(*args):
            # When openssl starts writing, the file is already private.
            if args[0] == "genrsa":
                target = Path(args[args.index("-out") + 1])
                seen[target.name] = stat.S_IMODE(target.stat().st_mode)
            return real(*args)

        previous = os.umask(0o000)
        try:
            with unittest.mock.patch.object(mesh, "_openssl", watching):
                mesh.init(self.root)
                mesh.cert(self.root, "database")
        finally:
            os.umask(previous)
        self.assertEqual(seen["ca.key"], 0o600)
        self.assertEqual(seen["database.key"], 0o600)
        self.assertEqual(
            stat.S_IMODE((self.root / "synqt" / "mesh" / "ca.key").stat().st_mode), 0o600)

    def test_a_name_that_is_not_an_entity_name_never_reaches_openssl(self):
        """A name typed at the prompt that is not an entity name never reaches openssl: it
        lands in file names, the subject `/CN=` and the SAN.
        """
        mesh.init(self.root)
        for name in ["../evil", "a/b", "..", "web edge", "CN=web/O=elsewhere", "", "9lives"]:
            with self.assertRaises(mesh.MeshError, msg=name):
                mesh.cert(self.root, name)
        self.assertEqual(sorted(p.name for p in self.root.glob("synqt/mesh/*.key")),
                         ["ca.key"])
        # And a name that IS one still works, so the rule is a shape and not a blocklist.
        mesh.cert(self.root, "web-edge_2")
        self.assertTrue((self.root / "synqt" / "mesh" / "web-edge_2.crt").exists())

    def test_status_reads_an_expiry_whatever_the_machine_locale_is(self):
        """`synqt mesh status` reads openssl's English month names under any locale."""
        with unittest.mock.patch.object(
                mesh, "_openssl", return_value="notAfter=Aug  4 12:34:56 2027 GMT\n"):
            parsed = mesh._not_after(Path("anything.crt"))
        self.assertEqual(parsed, datetime(2027, 8, 4, 12, 34, 56, tzinfo=timezone.utc))
        # An unreadable date is None, which status() reports, rather than an exception.
        with unittest.mock.patch.object(
                mesh, "_openssl", return_value="notAfter=Nonesuch 4 12:34:56 2027 GMT\n"):
            self.assertIsNone(mesh._not_after(Path("anything.crt")))

    def test_entity_certs_carry_the_key_usages_a_strict_verifier_requires(self):
        """An entity certificate chains to the CA and carries serverAuth and clientAuth.

        Asserted on the extensions, not with `openssl verify -purpose`: OpenSSL accepts a
        certificate with no EKU, while Apple's verifier requires the usage OID.
        """
        import subprocess

        mesh.init(self.root)
        mesh.cert(self.root, "database")
        mesh_dir = self.root / "synqt" / "mesh"
        ca = str(mesh_dir / "ca.crt")
        crt = str(mesh_dir / "database.crt")

        # It still has to chain to the CA at all.
        result = subprocess.run(["openssl", "verify", "-CAfile", ca, crt],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, f"{result.stdout}{result.stderr}")

        text = subprocess.check_output(
            ["openssl", "x509", "-noout", "-text", "-in", crt], text=True)
        self.assertIn("TLS Web Server Authentication", text)
        self.assertIn("TLS Web Client Authentication", text)
        # A leaf is not a CA.
        self.assertIn("CA:FALSE", text.replace(" ", ""))

    def test_the_ca_states_it_may_sign_certificates(self):
        """The CA states its own extensions; nothing is inherited from the host openssl.cnf."""
        import subprocess

        mesh.init(self.root)
        text = subprocess.check_output(
            ["openssl", "x509", "-noout", "-text", "-in",
             str(self.root / "synqt" / "mesh" / "ca.crt")], text=True)
        self.assertIn("CA:TRUE", text.replace(" ", ""))
        self.assertIn("Certificate Sign", text)

    def test_cert_requires_a_ca_and_status_reports_validity(self):
        with self.assertRaises(mesh.MeshError):
            mesh.cert(self.root, "web")
        mesh.init(self.root)
        mesh.cert(self.root, "web")
        report = mesh.status(self.root)
        self.assertIn("web: valid until", report)
        self.assertIn("ca: valid until", report)

    def test_rotate_issues_a_new_certificate_for_the_same_entity(self):
        """Rotation replaces the certificate on disk, for one entity or for the topology."""
        mesh.init(self.root)
        mesh.cert(self.root, "web")
        before = (self.root / "synqt" / "mesh" / "web.crt").read_bytes()

        self.assertIn("Issued web.crt", mesh.rotate(self.root, "web"))
        after = (self.root / "synqt" / "mesh" / "web.crt").read_bytes()
        self.assertNotEqual(before, after)

        # Without an entity it rotates every service entity, never the client.
        summary = mesh.rotate(self.root, service_entities=["web", "database"])
        self.assertIn("Issued web.crt", summary)
        self.assertIn("Issued database.crt", summary)
        self.assertTrue((self.root / "synqt" / "mesh" / "database.crt").exists())

        # No service entities: nothing to issue, and it says so.
        self.assertEqual(mesh.cert_all(self.root, []),
                         "No service entities in the topology.")

    def test_status_separates_expired_from_expiring_and_says_when_it_cannot_read_one(self):
        """The three states `synqt mesh status` reports. Dates come from a stubbed reader,
        since openssl's backdating options differ across versions.
        """
        mesh.init(self.root)
        mesh.cert(self.root, "web")
        now = datetime.now(timezone.utc)

        expiries = {"ca": now + timedelta(days=365), "web": now - timedelta(days=2)}
        with unittest.mock.patch.object(mesh, "_not_after",
                                        lambda crt: expiries.get(crt.stem)):
            report = mesh.status(self.root)
        # Expired, not "expires soon".
        self.assertIn("web: valid until", report)
        self.assertIn("<-- EXPIRED", report)
        self.assertNotIn("<-- EXPIRES SOON", report)
        # A certificate with a long life carries no flag at all.
        ca_line = next(line for line in report.splitlines() if line.strip().startswith("ca:"))
        self.assertNotIn("<--", ca_line)

        expiries["web"] = now + timedelta(days=5)
        with unittest.mock.patch.object(mesh, "_not_after",
                                        lambda crt: expiries.get(crt.stem)):
            self.assertIn("<-- EXPIRES SOON", mesh.status(self.root))

        # None: the file exists but its date could not be read.
        with unittest.mock.patch.object(mesh, "_not_after", lambda crt: None):
            report = mesh.status(self.root)
        self.assertIn("web: unreadable", report)
        self.assertNotIn("valid until", report)

    def test_status_before_the_ca_exists_names_the_command_that_creates_it(self):
        self.assertIn("Run 'synqt mesh init'", mesh.status(self.root))


class LicenseTest(unittest.TestCase):
    def test_client_wasm_is_gplv3_with_conveyance_note(self):
        text = licenses.generate({"name": "client", "type": "client"}, target="wasm")
        self.assertIn("Qt for WebAssembly platform: GPL-3.0-only", text)
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", text)
        self.assertIn("conveyed to every visitor", text)

    def test_desktop_client_is_lgplv3(self):
        text = licenses.generate({"name": "client", "type": "client"}, target="desktop")
        self.assertNotIn("WebAssembly platform", text)
        self.assertIn("LGPL-3.0-only", text)

    def test_web_edge_is_gplv3_pure_service_is_lgplv3(self):
        edge = licenses.generate({"name": "web", "type": "web_edge"})
        self.assertIn("Qt HTTP Server: GPL-3.0-only", edge)
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", edge)
        db = licenses.generate({"name": "database", "type": "relational"})
        self.assertIn("Qt Sql: LGPL-3.0-only", db)
        self.assertIn("Effective license of this entity artifact: LGPL-3.0-only", db)

    def test_commercial_mode(self):
        text = licenses.generate({"name": "web", "type": "web_edge"},
                                 qt_license_mode="commercial")
        self.assertIn("Commercial", text)

    def test_a_pure_service_names_no_gpl_module_and_no_jwt(self):
        """A pure service names no GPLv3-only module: `SynQtService` links neither Qt HTTP
        Server nor Qt Network Authorization.
        """
        for entity_type in ("relational", "cache", "document", "jobs", "service"):
            with self.subTest(entity_type):
                text = licenses.generate({"name": "store", "type": entity_type})
                self.assertNotIn("Qt HTTP Server", text)
                self.assertNotIn("Qt Network Authorization", text)
                self.assertNotIn("jwt-cpp", text)
                self.assertIn("Effective license of this entity artifact: LGPL-3.0-only",
                              text)

    def test_the_auth_entity_reports_what_promoting_identity_made_it_link(self):
        """The auth entity is GPLv3 only because `identity.provider_entity` names it; without
        the config it reads as a plain LGPLv3 service.
        """
        auth = {"name": "auth", "type": "service"}
        config = {"entities": [{"name": "web", "type": "web_edge"}, auth],
                  "identity": {"provider_entity": "auth",
                               "providers": [{"name": "github"}]}}
        text = licenses.generate(auth, config=config)
        self.assertIn("Qt Network Authorization: GPL-3.0-only", text)
        self.assertIn("jwt-cpp: MIT", text)
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", text)
        # The edge runs the routes, not the engine, so the auth entity links no HTTP server.
        self.assertNotIn("Qt HTTP Server", text)
        self.assertIn("LGPL-3.0-only", licenses.generate(auth))

    def test_the_file_and_the_build_read_the_same_table(self):
        """A GPLv3-only module is named if and only if the entity links it. `licenses.py` and
        `cmakegen.py` both use `appmodel.service_libraries`.
        """
        config = {"entities": [{"name": "web", "type": "web_edge"},
                               {"name": "auth", "type": "service"},
                               {"name": "database", "type": "relational"},
                               {"name": "gateway", "type": "api",
                                "network": {"inbound": {"port": 8443,
                                                        "api_keys": "env:K"}}}],
                  "identity": {"provider_entity": "auth",
                               "providers": [{"name": "github"}]}}
        for entity in config["entities"]:
            with self.subTest(entity["name"]):
                expected = [module
                            for library in appmodel.service_libraries(config, entity)
                            for module in appmodel.LIBRARY_GPL_MODULES[library]]
                modules = licenses.entity_modules(entity, config=config)
                self.assertEqual([m for m in modules
                                  if licenses._MODULE_LICENSE[m] == "GPL-3.0-only"],
                                 [m for m in modules if m in expected])
                for module in expected:
                    self.assertIn(module, modules)

    def test_the_client_notice_names_every_module_the_client_links(self):
        """The client notice lists every Qt component cmakegen links into the client."""
        config = {"project": {"name": "app", "qt_version": "6.12.0"},
                  "entities": [{"name": "app", "type": "client"},
                               {"name": "edge", "type": "web_edge"}]}
        text = cmakegen.render_root_cmakelists(config, Path("/tmp/synqt"))
        block = re.search(r"target_link_libraries\(app PRIVATE(.*?)\)", text, re.DOTALL)
        self.assertIsNotNone(block, "no client link line to read")
        linked = set(re.findall(r"Qt6::[A-Za-z0-9_]+", block.group(1)))
        self.assertTrue(linked, "the client link line names no Qt module")

        unknown = linked - set(licenses.CLIENT_MODULES)
        self.assertFalse(unknown, f"linked into the client and not in CLIENT_MODULES: "
                                  f"{sorted(unknown)}")
        modules = licenses.entity_modules(config["entities"][0], target="wasm")
        for component in sorted(linked):
            self.assertIn(licenses.CLIENT_MODULES[component], modules)


class CheckTest(unittest.TestCase):
    def _base(self):
        return {"entities": [{"name": "client", "type": "client"},
                             {"name": "web", "type": "web_edge"},
                             {"name": "database", "type": "relational"}]}

    def test_client_consuming_a_non_edge_connect_point_fails(self):
        config = self._base()
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web", "client"]}]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("browser can only reach a web edge" in m for m in messages))

    def test_valid_topology_passes(self):
        config = self._base()
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web"]},
            {"owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)

    def test_explicit_local_link_is_flagged_but_not_an_error(self):
        # Every local link is reported, but an explicit one does not fail the build (pitfall
        # 7).
        config = self._base()
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web"],
             "transport": "local", "transport_local_explicit": True},
            {"owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertTrue(any(m.startswith("warn:") and "colocation-trusted" in m
                            for m in messages))

    def test_a_local_link_with_two_consumers_is_an_error(self):
        # A local point with two consumers is refused: both would be named after the first.
        config = self._base()
        config["entities"].append({"name": "reporter", "type": "jobs"})
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web", "reporter"],
             "transport": "local", "transport_local_explicit": True},
            {"owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("cannot tell them apart" in m and "'web'" in m
                            for m in messages), messages)

    def test_implicit_local_link_is_an_error(self):
        config = self._base()
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web"],
             "transport": "local", "transport_local_explicit": False},
            {"owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("transport local implicitly" in m for m in messages))

    def test_unknown_client_logging_mode_is_an_error(self):
        config = self._base()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        config["build"] = {"client_logging": "verbose"}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("build.client_logging must be" in m for m in messages))

    def test_valid_client_logging_mode_passes(self):
        config = self._base()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        config["build"] = {"client_logging": "none"}
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)

    def test_set_based_scopes_pass(self):
        # `scopes.hierarchical: false` is honoured by both mains and validates clean.
        config = self._base()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        config["scopes"] = {"order": ["anonymous", "user"], "hierarchical": False}
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)

    def test_non_boolean_scopes_hierarchical_is_an_error(self):
        # The string "false" is refused.
        config = self._base()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        config["scopes"] = {"order": ["anonymous", "user"], "hierarchical": "false"}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("scopes.hierarchical must be true or false" in m
                            for m in messages))


class NewBuildDoctorTest(unittest.TestCase):
    def setUp(self):
        self.parent = Path(tempfile.mkdtemp())

    def test_new_scaffolds_a_runnable_topology_and_warns_about_gpl(self):
        message = newproject.scaffold(self.parent, "app")
        root = self.parent / "app"
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        names = {e["name"] for e in config["entities"]}
        self.assertEqual(names, {"app", "edge"})
        self.assertTrue((root / "client" / "app" / "Main.qml").exists())
        # Presets sit at the project root, where CMake reads them, beside the four-line root
        # CMakeLists.txt.
        self.assertTrue((root / "CMakePresets.json").exists())
        self.assertIn("generated/synqt.cmake", (root / "CMakeLists.txt").read_text())
        self.assertTrue((root / "generated" / "synqt.cmake").exists())
        # Nothing else generated lands in the folders their authors write in.
        self.assertFalse((root / "web" / "edge" / "main.cpp").exists())
        self.assertIn("generated/", (root / ".gitignore").read_text())
        self.assertIn("synqt/mesh/*.key", (root / ".gitignore").read_text())
        self.assertIn("GPLv3", message)  # the conveyance reminder

    def test_build_emits_per_entity_dirs_with_accurate_licenses(self):
        newproject.scaffold(self.parent, "app", auth="github")
        root = self.parent / "app"
        summary = buildmod.build(root, profile_name="release", client="wasm")
        self.assertTrue((root / "build" / "client" / "THIRD-PARTY-LICENSES").exists())
        self.assertTrue((root / "build" / "edge" / "THIRD-PARTY-LICENSES").exists())
        # The edge that runs identity links Network Authorization -> GPLv3, jwt-cpp noted.
        web_license = (root / "build" / "edge" / "THIRD-PARTY-LICENSES").read_text()
        self.assertIn("Network Authorization: GPL-3.0-only", web_license)
        self.assertIn("jwt-cpp: MIT", web_license)
        self.assertIn("GPLv3", summary)

    def test_doctor_reports_license_mode_and_missing_ca(self):
        newproject.scaffold(self.parent, "app")
        report = doctor.report(self.parent / "app")
        self.assertIn("Qt license mode: open_source", report)
        self.assertIn("no production CA", report)
        self.assertIn("GPLv3", report)

    def test_doctor_reports_the_sql_driver_plugin_a_provider_needs(self):
        """doctor reports the Qt SQL driver plugin a provider needs, and does not claim `synqt
        build` provides it.
        """
        newproject.scaffold(self.parent, "app")
        root = self.parent / "app"
        config = configmod.load(root)
        config.setdefault("entities", []).extend([
            {"name": "store", "type": "service", "provider": {"name": "mysql"}},
            {"name": "warehouse", "type": "service", "provider": {"name": "postgres"}},
            {"name": "hot", "type": "service", "provider": {"name": "redis"}},
        ])
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

        report = doctor.report(root)
        # postgres. The plugin question, without the mysql licensing tail.
        self.assertIn("Provider 'postgres' on entity 'warehouse'", report)
        self.assertIn("QPSQL", report)
        # redis. A compile-time dependency instead, so no plugin line at all.
        self.assertIn("Provider 'redis' on entity 'hot'", report)
        self.assertIn("hiredis", report)
        self.assertNotIn("QREDIS", report)

        self.assertIn("Provider 'mysql' on entity 'store'", report)
        self.assertIn("QMYSQL", report)
        self.assertIn("MariaDB Connector/C", report)
        # The licensing reason is printed with the instruction.
        self.assertIn("libmysqlclient", report)
        self.assertIn("build-qmysql-plugin.sh", report)
        self.assertNotIn("synqt build resolves it", report)

    def test_doctor_reads_the_plugin_from_the_resolved_kit_not_the_host(self):
        """The same two providers against a kit the test lays down with both plugin files.
        QMYSQL is still reported as unusable: the shipped plugin links Oracle's
        libmysqlclient.
        """
        newproject.scaffold(self.parent, "app")
        root = self.parent / "app"
        # The project toolchain directory wins over any system Qt.
        drivers = (root / "synqt" / "toolchain" / "qt" / toolchain.QT_VERSION
                   / toolchain.host_kit_dir() / "plugins" / "sqldrivers")
        drivers.mkdir(parents=True)
        for stem in ("libqsqlpsql.so", "libqsqlmysql.so"):
            (drivers / stem).write_bytes(b"")
        config = configmod.load(root)
        config.setdefault("entities", []).extend([
            {"name": "warehouse", "type": "service", "provider": {"name": "postgres"}},
            {"name": "store", "type": "service", "provider": {"name": "mysql"}},
        ])
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

        report = doctor.report(root)
        self.assertIn(f"QPSQL plugin: present ({drivers / 'libqsqlpsql.so'})", report)
        self.assertNotIn("QPSQL plugin: not in the resolved Qt kit", report)
        self.assertIn("QMYSQL plugin: a plugin file is in the Qt kit", report)
        self.assertIn("settles nothing on its own", report)
        self.assertNotIn("QMYSQL plugin: present", report)


class BuildEntitySelectionTest(unittest.TestCase):
    """`synqt build --entity <name>` builds one entity. Two builds only, since each compiles a
    real WebAssembly client when both kits are present.
    """

    def setUp(self):
        self.parent = Path(tempfile.mkdtemp())
        newproject.scaffold(self.parent, "app")
        self.root = self.parent / "app"

    def test_an_unknown_entity_is_an_error_naming_the_real_ones(self):
        # An unknown name raises before any compilation.
        with self.assertRaises(buildmod.BuildError) as caught:
            buildmod.build(self.root, entity="databse")
        self.assertIn("app", str(caught.exception))
        self.assertIn("edge", str(caught.exception))

    def test_a_build_names_every_entity_whose_store_does_not_outlive_it(self):
        """A build names every entity it produced whose store is in memory (the document type
        default), and only those.
        """
        from synqt import build as buildmod

        memory_doc = {"name": "notes", "type": "document"}
        named_memory = {"name": "drafts", "type": "document",
                        "provider": {"name": "memory"}}
        on_mongo = {"name": "records", "type": "document",
                    "provider": {"name": "mongodb"}}
        relational = {"name": "books", "type": "relational"}

        notices = buildmod.volatile_store_notices([memory_doc, named_memory, on_mongo,
                                                   relational])
        self.assertEqual(len(notices), 2)
        self.assertIn("'notes' keeps its documents in memory", notices[0])
        self.assertIn("'drafts' keeps its documents in memory", notices[1])
        for notice in notices:
            self.assertIn("gone when the process stops", notice)
        self.assertEqual(buildmod.volatile_store_notices([on_mongo, relational]), [])

    def test_a_failed_compile_is_an_error_and_never_a_summary_bullet(self):
        # A failed compile raises instead of reporting success. Driven through
        # _compile_failure, since reaching the compile path needs a Qt toolchain.
        error = subprocess.CalledProcessError(
            returncode=1, cmd=["cmake", "--preset", "host"],
            stderr="CMake Error at src/identity/CMakeLists.txt:29 (message):\n"
                   "  jwt-cpp not found.\n"
                   "-- Configuring incomplete, errors occurred!\n")
        message = buildmod._compile_failure(error, verbose=False)
        # The cause survives, not only cmake's "Configuring incomplete, errors occurred!".
        self.assertIn("jwt-cpp not found.", message)
        self.assertIn("cmake --preset host", message)

    def test_a_failed_compile_quotes_the_diagnostics_ninja_wrote_to_stdout(self):
        # stdout is read too: Ninja writes "FAILED:" and the compiler diagnostics there.
        error = subprocess.CalledProcessError(
            returncode=1, cmd=["cmake", "--build", "build/host-debug", "--target", "edge"],
            output="[3/9] Building CXX object edge/CMakeFiles/edge.dir/main.cpp.o\n"
                   "FAILED: edge/CMakeFiles/edge.dir/main.cpp.o\n"
                   "main.cpp:12:5: error: use of undeclared identifier 'Caller'\n"
                   "ninja: build stopped: subcommand failed.\n")
        message = buildmod._compile_failure(error, verbose=False)
        self.assertIn("use of undeclared identifier 'Caller'", message)
        self.assertIn("--target edge", message)

    def test_a_failed_compile_with_no_captured_output_still_names_the_command(self):
        error = subprocess.CalledProcessError(returncode=1, cmd=["cmake", "--build", "x"],
                                              stderr="")
        message = buildmod._compile_failure(error, verbose=False)
        self.assertIn("cmake --build x", message)
        # And the exit code, which is all that is left to say when both streams are empty.
        self.assertIn("1", message)

    def test_the_compiled_note_names_the_targets_it_actually_built(self):
        # With --entity the note names the targets. Asserted on the wording, which needs no
        # toolchain.
        self.assertEqual(buildmod.built_note(["web"], []),
                         "compiled web through the pinned toolchain.")
        self.assertEqual(buildmod.built_note(["web", "database"], ["wasm"]),
                         "compiled web, database, client (wasm) through the pinned toolchain.")
        # A desktop client is a host target, not counted twice.
        self.assertEqual(buildmod.built_note(["client"], ["desktop"]),
                         "compiled client through the pinned toolchain.")
        self.assertEqual(buildmod.built_note([], []), "nothing to compile.")

    def test_an_edge_only_build_makes_no_client_and_says_so(self):
        summary = buildmod.build(self.root, entity="edge")
        self.assertTrue((self.root / "build" / "edge" / "THIRD-PARTY-LICENSES").exists())
        self.assertFalse((self.root / "build" / "client").exists())
        self.assertIn("Built 1 entity artifact(s)", summary)
        # No client GPLv3 reminder when no client was built.
        self.assertNotIn("served to every visitor", summary)
        self.assertIn("distributing the edge binary", summary)

    def test_a_client_only_build_warns_about_conveyance_and_not_the_edge(self):
        summary = buildmod.build(self.root, entity="app")
        self.assertFalse((self.root / "build" / "edge").exists())
        self.assertIn("served to every visitor", summary)
        self.assertNotIn("distributing the edge binary", summary)


if __name__ == "__main__":
    unittest.main()
