# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""M10: the mesh CA tooling, the license generation, and the new/check/build/doctor flow."""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import build as buildmod
from synqt import check, doctor, licenses, mesh, newproject


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
        # -nameopt RFC2253 rather than openssl's default rendering: that default is not
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

    def test_entity_certs_carry_the_key_usages_a_strict_verifier_requires(self):
        """An entity cert must chain to the CA and state both TLS roles it plays: server
        on the links it owns, client on the links it consumes.

        The usages are asserted as extensions rather than through `openssl verify
        -purpose`, which cannot see this defect. The shipped bug was certificates issued
        with no extendedKeyUsage at all; OpenSSL only enforces an EKU that is *present*,
        so it returns OK for both sslserver and sslclient on an EKU-less cert (measured,
        not assumed). Apple's verifier instead requires the matching usage OID, so those
        certs were fine on Linux and untrusted on macOS -- every suite that verified a
        peer failed there, and the ones that skipped verification passed. An EKU-less
        cert is not "unrestricted" everywhere; it is unusable on a strict verifier.

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


class LicenseTest(unittest.TestCase):
    def test_client_wasm_is_gplv3_with_conveyance_note(self):
        text = licenses.generate({"name": "client", "kind": "client"}, target="wasm")
        self.assertIn("Qt for WebAssembly platform: GPL-3.0-only", text)
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", text)
        self.assertIn("conveyed to every visitor", text)

    def test_desktop_client_is_lgplv3(self):
        text = licenses.generate({"name": "client", "kind": "client"}, target="desktop")
        self.assertNotIn("WebAssembly platform", text)
        self.assertIn("LGPL-3.0-only", text)

    def test_web_edge_is_gplv3_pure_service_is_lgplv3(self):
        edge = licenses.generate({"name": "web", "capability": "web_edge"})
        self.assertIn("Qt HTTP Server: GPL-3.0-only", edge)
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", edge)
        db = licenses.generate({"name": "database", "kind": "service", "blueprint": "persistence"})
        self.assertIn("Qt Sql: LGPL-3.0-only", db)
        self.assertIn("Effective license of this entity artifact: LGPL-3.0-only", db)

    def test_commercial_mode(self):
        text = licenses.generate({"name": "web", "capability": "web_edge"},
                                 qt_license_mode="commercial")
        self.assertIn("Commercial", text)


class CheckTest(unittest.TestCase):
    def _base(self):
        return {"entities": [{"name": "client", "kind": "client"},
                             {"name": "web", "kind": "service", "capability": "web_edge"},
                             {"name": "database", "kind": "service", "blueprint": "persistence"}]}

    def test_client_consuming_a_non_edge_connect_point_fails(self):
        config = self._base()
        config["connect_points"] = [
            {"name": "items", "owner": "database", "consumers": ["web", "client"]}]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("browser can only reach a web edge" in m for m in messages))

    def test_valid_topology_passes(self):
        config = self._base()
        config["connect_points"] = [
            {"name": "items", "owner": "database", "consumers": ["web"]},
            {"name": "todo", "owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)

    def test_explicit_local_link_is_flagged_but_not_an_error(self):
        # Every local link is surfaced (colocation-trusted, not authenticated) yet a
        # legitimately opted-in one does not fail the build (pitfall 7).
        config = self._base()
        config["connect_points"] = [
            {"name": "items", "owner": "database", "consumers": ["web"],
             "transport": "local", "transport_local_explicit": True},
            {"name": "todo", "owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertTrue(any(m.startswith("warn:") and "colocation-trusted" in m
                            for m in messages))

    def test_implicit_local_link_is_an_error(self):
        config = self._base()
        config["connect_points"] = [
            {"name": "items", "owner": "database", "consumers": ["web"],
             "transport": "local", "transport_local_explicit": False},
            {"name": "todo", "owner": "web", "consumers": ["client"]}]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("transport local implicitly" in m for m in messages))

    def test_unknown_client_logging_mode_is_an_error(self):
        config = self._base()
        config["connect_points"] = [
            {"name": "todo", "owner": "web", "consumers": ["client"]}]
        config["build"] = {"client_logging": "verbose"}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("build.client_logging must be" in m for m in messages))

    def test_valid_client_logging_mode_passes(self):
        config = self._base()
        config["connect_points"] = [
            {"name": "todo", "owner": "web", "consumers": ["client"]}]
        config["build"] = {"client_logging": "none"}
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)


class NewBuildDoctorTest(unittest.TestCase):
    def setUp(self):
        self.parent = Path(tempfile.mkdtemp())

    def test_new_scaffolds_a_runnable_topology_and_warns_about_gpl(self):
        message = newproject.scaffold(self.parent, "app")
        root = self.parent / "app"
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        names = {e["name"] for e in config["entities"]}
        self.assertEqual(names, {"client", "web"})
        self.assertTrue((root / "client" / "Main.qml").exists())
        self.assertTrue((root / "CMakePresets.json").exists())
        self.assertIn("synqt/mesh/*.key", (root / ".gitignore").read_text())
        self.assertIn("GPLv3", message)  # the conveyance reminder

    def test_build_emits_per_entity_dirs_with_accurate_licenses(self):
        newproject.scaffold(self.parent, "app", auth="github")
        root = self.parent / "app"
        summary = buildmod.build(root, release=True, client="wasm")
        self.assertTrue((root / "build" / "client" / "THIRD-PARTY-LICENSES").exists())
        self.assertTrue((root / "build" / "web" / "THIRD-PARTY-LICENSES").exists())
        # The edge that runs identity links Network Authorization -> GPLv3, jwt-cpp noted.
        web_license = (root / "build" / "web" / "THIRD-PARTY-LICENSES").read_text()
        self.assertIn("Network Authorization: GPL-3.0-only", web_license)
        self.assertIn("jwt-cpp: MIT", web_license)
        self.assertIn("GPLv3", summary)

    def test_doctor_reports_license_mode_and_missing_ca(self):
        newproject.scaffold(self.parent, "app")
        report = doctor.report(self.parent / "app")
        self.assertIn("Qt license mode: open_source", report)
        self.assertIn("no production CA", report)
        self.assertIn("GPLv3", report)


class BuildEntitySelectionTest(unittest.TestCase):
    """`synqt build --entity <name>`: build one entity instead of the whole system.

    Two builds, not one per assertion: on a machine with both Qt kits every build() here
    compiles a real WebAssembly client, so a test per claim costs minutes. The whole-system
    build is covered by NewBuildDoctorTest above; these two cover what --entity changes.
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
        self.assertIn("client", str(caught.exception))
        self.assertIn("web", str(caught.exception))

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
            stderr="CMake Error at src/service/CMakeLists.txt:29 (message):\n"
                   "  jwt-cpp not found.\n"
                   "-- Configuring incomplete, errors occurred!\n")
        message = buildmod._compile_failure(error, verbose=False)
        # The cause has to survive, not just the last line: cmake signs off with
        # "Configuring incomplete, errors occurred!", which names nothing at all.
        self.assertIn("jwt-cpp not found.", message)
        self.assertIn("cmake --preset host", message)

    def test_a_failed_compile_with_no_captured_output_still_names_the_command(self):
        error = subprocess.CalledProcessError(returncode=1, cmd=["cmake", "--build", "x"],
                                              stderr="")
        message = buildmod._compile_failure(error, verbose=False)
        self.assertIn("cmake --build x", message)

    def test_the_compiled_note_names_the_targets_it_actually_built(self):
        # The rule this pins: with --entity, the note is the only thing that says the build
        # was partial, so it names the targets. Asserted on the wording itself rather than
        # on a real build's summary, because reaching the compile path needs a resolved Qt
        # toolchain; a machine without one skips compilation, and the assertion would then
        # only ever be testing whether Qt happened to be installed.
        self.assertEqual(buildmod.built_note(["web"], []),
                         "compiled web through the pinned toolchain.")
        self.assertEqual(buildmod.built_note(["web", "database"], ["wasm"]),
                         "compiled web, database, client (wasm) through the pinned toolchain.")
        # A desktop client is a host target, so it is already named; it must not be counted
        # twice as a client target too.
        self.assertEqual(buildmod.built_note(["client"], ["desktop"]),
                         "compiled client through the pinned toolchain.")
        self.assertEqual(buildmod.built_note([], []), "nothing to compile.")

    def test_an_edge_only_build_makes_no_client_and_says_so(self):
        summary = buildmod.build(self.root, entity="web")
        self.assertTrue((self.root / "build" / "web" / "THIRD-PARTY-LICENSES").exists())
        self.assertFalse((self.root / "build" / "client").exists())
        self.assertIn("Built 1 entity artifact(s)", summary)
        # The client GPLv3 reminder is about the client artifact, and no client was built.
        # A warning that appears when it does not apply is one people learn to skim, and
        # this is the warning that must not be skimmed.
        self.assertNotIn("served to every visitor", summary)
        self.assertIn("distributing the edge binary", summary)

    def test_a_client_only_build_warns_about_conveyance_and_not_the_edge(self):
        summary = buildmod.build(self.root, entity="client")
        self.assertFalse((self.root / "build" / "web").exists())
        self.assertIn("served to every visitor", summary)
        self.assertNotIn("distributing the edge binary", summary)


if __name__ == "__main__":
    unittest.main()
