# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""M10: the mesh CA tooling, the license generation, and the new/check/build/doctor flow."""

import os
import stat
import subprocess
import tempfile
import unittest
import unittest.mock
from datetime import datetime, timedelta, timezone
from pathlib import Path

import yaml

from synqt import build as buildmod
from synqt import appmodel
from synqt import check, config as configmod, doctor, licenses, mesh, newproject, toolchain


class MeshTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())

    def test_ca_key_is_private_and_gitignored_never_a_client_cert(self):
        mesh.init(self.root)
        ca_key = self.root / "synqt" / "mesh" / "ca.key"
        self.assertTrue(ca_key.exists())
        # The CA private key is 0600. Only where the mode means something: Windows has no
        # POSIX bits, os.chmod there only toggles read-only, and stat reports 0666 whatever
        # was asked for. mesh._restrict() carries the Windows half (an ACL), which this
        # cannot assert from a POSIX box.
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(ca_key.stat().st_mode), 0o600)
        # ...and git-ignored, so it is never committed.
        self.assertIn("synqt/mesh/*.key", (self.root / ".gitignore").read_text())

        # An entity certificate carries the entity name as its subject. Read it back with
        # -nameopt RFC2253 rather than openssl's default rendering. That default is not
        # stable across openssl versions ("CN=database" here, "CN = database" on the CI
        # image), so an unpinned format tests the local openssl build, not the subject.
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
        """The key file is created with its final mode, not chmod-ed into it afterwards.

        `openssl genrsa -out` creates the file with the process umask, which on a default
        system is 0644, and the `chmod` that follows closes a window rather than never
        opening it. In that window every account on the machine can read the trust anchor
        for every mesh link in the project.

        Asserted by running the generation with a permissive umask, which is what makes the
        window observable. Without the fix the file is left 0666 & ~umask at creation, and
        the assertion below is on the mode openssl itself wrote, taken from a callable that
        stands in for it.
        """
        if os.name == "nt":
            self.skipTest("POSIX modes; mesh._restrict carries the Windows ACL half")
        seen = {}
        real = mesh._openssl

        def watching(*args):
            # The moment openssl would start writing the key is the moment the file has to
            # be private already.
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


    def test_entity_certs_carry_the_key_usages_a_strict_verifier_requires(self):
        """An entity cert must chain to the CA and state both TLS roles it plays: server
        on the links it owns, client on the links it consumes.

        The usages are asserted as extensions rather than through `openssl verify
        -purpose`, which cannot see this defect. The shipped bug was certificates issued
        with no extendedKeyUsage at all; OpenSSL only enforces an EKU that is *present*,
        so it returns OK for both sslserver and sslclient on an EKU-less cert (measured,
        not assumed). Apple's verifier instead requires the matching usage OID, so those
        certs were fine on Linux and untrusted on macOS; every suite that verified a
        peer failed there, and the ones that skipped verification passed. An EKU-less
        cert is not "unrestricted" everywhere. It is unusable on a strict verifier.

        Since the platform that enforces this cannot be run from here, the extension is
        the thing to assert. Keep this test string-based for that reason.
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
        # A leaf must not also be a CA: a verifier that enforces basic constraints has to
        # see an end entity, and nothing must be able to sign with it.
        self.assertIn("CA:FALSE", text.replace(" ", ""))

    def test_the_ca_states_it_may_sign_certificates(self):
        """The CA is the trust anchor for every mesh link, so what it is allowed to do is
        part of the tool's contract rather than of the host's openssl.cnf."""
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
        """Rotation replaces the certificate on disk, for one entity or for the topology.

        The point of the command is that the old key stops being the one the mesh trusts,
        so the file has to change. A rotate that reissued nothing would report
        success and leave a compromised key in service.
        """
        mesh.init(self.root)
        mesh.cert(self.root, "web")
        before = (self.root / "synqt" / "mesh" / "web.crt").read_bytes()

        self.assertIn("Issued web.crt", mesh.rotate(self.root, "web"))
        after = (self.root / "synqt" / "mesh" / "web.crt").read_bytes()
        self.assertNotEqual(before, after)

        # Without an entity it rotates every service entity it is given, and says so for
        # each. The client is not among them. It never holds a mesh certificate.
        summary = mesh.rotate(self.root, service_entities=["web", "database"])
        self.assertIn("Issued web.crt", summary)
        self.assertIn("Issued database.crt", summary)
        self.assertTrue((self.root / "synqt" / "mesh" / "database.crt").exists())

        # A topology with no service entities is not an error. There is nothing to
        # issue, and saying that beats printing an empty line that reads like a failure.
        self.assertEqual(mesh.cert_all(self.root, []),
                         "No service entities in the topology.")

    def test_status_separates_expired_from_expiring_and_says_when_it_cannot_read_one(self):
        """The three states `synqt mesh status` exists to tell apart.

        Dates come from a stubbed reader rather than from certificates issued with a past
        validity: openssl's options for backdating are not the same across the versions
        this suite runs on, and the question here is what status reports for a given
        expiry, not whether openssl can be talked into producing one.
        """
        mesh.init(self.root)
        mesh.cert(self.root, "web")
        now = datetime.now(timezone.utc)

        expiries = {"ca": now + timedelta(days=365), "web": now - timedelta(days=2)}
        with unittest.mock.patch.object(mesh, "_not_after",
                                        lambda crt: expiries.get(crt.stem)):
            report = mesh.status(self.root)
        # Expired, not "expires soon": a mesh that is already down reads differently from
        # one that needs attention this month.
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

        # None means the file is there but openssl could not read a date out of it, which
        # is a different problem from an expiry and must not be reported as one.
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
        """The LGPLv3 line has to be a fact about the binary, not a hope.

        `SynQtService` links neither Qt HTTP Server nor Qt Network Authorization, so a
        relational, cache, document, jobs or plain service entity carries neither, and its
        file must not claim otherwise either way.
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
        """`identity.provider_entity` is the one thing that makes a service GPLv3.

        Nothing on the entity itself says it runs the login. The project block names it. So
        the file is generated with the config, and without it the same entity is an ordinary
        LGPLv3 service, which is what it is in every project that does not promote identity.
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
        """A GPLv3-only module is named if and only if the entity links the library with it.

        `licenses.py` and `cmakegen.py` both go through `appmodel.service_libraries`. This
        is what stops the two from drifting. The old file keyed the auth obligations off a
        field nothing wrote, and reported LGPL for entities that were linking HttpServer.
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
        # Every local link is surfaced (colocation-trusted, not authenticated) yet a
        # legitimately opted-in one does not fail the build (pitfall 7).
        config = self._base()
        config["connect_points"] = [
            {"owner": "database", "consumers": ["web"],
             "transport": "local", "transport_local_explicit": True},
            {"owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertTrue(any(m.startswith("warn:") and "colocation-trusted" in m
                            for m in messages))

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
        # scopes.hierarchical. False is a legitimate, fully-honored setting (both mains
        # emit it now), so it must validate clean.
        config = self._base()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        config["scopes"] = {"order": ["anonymous", "user"], "hierarchical": False}
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)

    def test_non_boolean_scopes_hierarchical_is_an_error(self):
        # The string "false" is truthy in Python, so a quoted value would silently stay
        # hierarchical, the authorization surprise the setter meant to turn off. Refuse it.
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
        # CMake reads presets from the top-level source directory and nowhere else, and
        # that directory is the project. The root CMakeLists.txt beside them is four lines
        # and hands the build to generated/synqt.cmake.
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
        summary = buildmod.build(root, release=True, client="wasm")
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
        """A SQL-backed provider needs a Qt driver plugin at run time, and doctor is the
        only place that says so before the first query fails. It must not claim `synqt
        build` resolves it, which would be wrong twice. The build installs no engine client,
        and it does not produce the plugin either."""
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
        # The licensing reason, not only the instruction. It is why the shipped plugin is
        # unusable rather than merely inconvenient.
        self.assertIn("libmysqlclient", report)
        self.assertIn("build-qmysql-plugin.sh", report)
        self.assertNotIn("synqt build resolves it", report)

    def test_doctor_reads_the_plugin_from_the_resolved_kit_not_the_host(self):
        """The same two providers again, against a kit that has both plugin files.

        The kit is built here rather than borrowed from the machine, for two reasons.
        A developer box with Qt installed and a CI runner without it are opposite halves
        of this function, and only one of them can be the environment the suite happens to
        run in. A kit the test lays down itself exercises both halves everywhere. And the
        mysql wording is the point. A plugin file being there settles nothing, because the
        shipped one is linked against Oracle's libmysqlclient, so doctor must not report it
        the way it reports QPSQL.
        """
        newproject.scaffold(self.parent, "app")
        root = self.parent / "app"
        # The project's own toolchain directory wins over any system Qt, so this resolves
        # the same on a machine with a kit installed and on one without.
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
    """`synqt build --entity <name>`: build one entity instead of the whole system.

    Two builds, not one per assertion: on a machine with both Qt kits every build() here
    compiles a real WebAssembly client, so a test per claim costs minutes. The whole-system
    build is covered by NewBuildDoctorTest above. These two cover what --entity changes.
    """

    def setUp(self):
        self.parent = Path(tempfile.mkdtemp())
        newproject.scaffold(self.parent, "app")
        self.root = self.parent / "app"

    def test_an_unknown_entity_is_an_error_naming_the_real_ones(self):
        # Not an empty build: "Built 0 entity artifact(s)" for a typo is a success message
        # for work that never happened. Raises before any compilation, so this one is cheap.
        with self.assertRaises(buildmod.BuildError) as caught:
            buildmod.build(self.root, entity="databse")
        self.assertIn("app", str(caught.exception))
        self.assertIn("edge", str(caught.exception))

    def test_a_failed_compile_is_an_error_and_never_a_summary_bullet(self):
        # The rule: `synqt build` must not report success for a build that did not happen.
        # It used to. A failing cmake was caught, turned into a note, appended to a "Built N
        # entity artifact(s)" summary, printed, and the command exited 0 -- having also written
        # each named artifact a THIRD-PARTY-LICENSES describing a binary that did not exist.
        # CI ran that way for as long as this command has existed: the arena's edge failed to
        # configure for a missing dependency and the job went red only because a later step
        # looked for the bundle on disk and did not find it.
        #
        # Driven through _compile_failure rather than a real broken build, for the reason the
        # note test below gives: reaching the compile path needs a resolved Qt toolchain.
        error = subprocess.CalledProcessError(
            returncode=1, cmd=["cmake", "--preset", "host"],
            stderr="CMake Error at src/identity/CMakeLists.txt:29 (message):\n"
                   "  jwt-cpp not found.\n"
                   "-- Configuring incomplete, errors occurred!\n")
        message = buildmod._compile_failure(error, verbose=False)
        # The cause has to survive, not only the last line: cmake signs off with
        # "Configuring incomplete, errors occurred!", which names nothing at all.
        self.assertIn("jwt-cpp not found.", message)
        self.assertIn("cmake --preset host", message)

    def test_a_failed_compile_quotes_the_diagnostics_ninja_wrote_to_stdout(self):
        # The half that was missing, and it is the half the common failure uses.
        # `cmake --build` hands the work to Ninja, which writes its "FAILED:" line and the
        # compiler diagnostics under it to stdout; stderr stays empty. Reading stderr alone
        # meant that every broken compile -- the ordinary case, as against a broken
        # configure -- reported "no output captured" and named the command, which is the one
        # thing the reader already had.
        error = subprocess.CalledProcessError(
            returncode=1, cmd=["cmake", "--build", "build/host", "--target", "edge"],
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
        # The rule this pins. With --entity, the note is the only thing that says the build
        # was partial, so it names the targets. Asserted on the wording itself rather than
        # on a real build's summary, because reaching the compile path needs a resolved Qt
        # toolchain. A machine without one skips compilation, and the assertion would then
        # only ever be testing whether Qt happened to be installed.
        self.assertEqual(buildmod.built_note(["web"], []),
                         "compiled web through the pinned toolchain.")
        self.assertEqual(buildmod.built_note(["web", "database"], ["wasm"]),
                         "compiled web, database, client (wasm) through the pinned toolchain.")
        # A desktop client is a host target, so it is already named. It must not be counted
        # twice as a client target too.
        self.assertEqual(buildmod.built_note(["client"], ["desktop"]),
                         "compiled client through the pinned toolchain.")
        self.assertEqual(buildmod.built_note([], []), "nothing to compile.")

    def test_an_edge_only_build_makes_no_client_and_says_so(self):
        summary = buildmod.build(self.root, entity="edge")
        self.assertTrue((self.root / "build" / "edge" / "THIRD-PARTY-LICENSES").exists())
        self.assertFalse((self.root / "build" / "client").exists())
        self.assertIn("Built 1 entity artifact(s)", summary)
        # The client GPLv3 reminder is about the client artifact, and no client was built.
        # A warning that appears when it does not apply is one people learn to skim, and
        # this is the warning that must not be skimmed.
        self.assertNotIn("served to every visitor", summary)
        self.assertIn("distributing the edge binary", summary)

    def test_a_client_only_build_warns_about_conveyance_and_not_the_edge(self):
        summary = buildmod.build(self.root, entity="app")
        self.assertFalse((self.root / "build" / "edge").exists())
        self.assertIn("served to every visitor", summary)
        self.assertNotIn("distributing the edge binary", summary)


if __name__ == "__main__":
    unittest.main()
