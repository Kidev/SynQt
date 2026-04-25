# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The development sign-in (`identity.dev_stub`), what it configures, what it refuses,
and the three gates that keep it out of anything that ships."""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import addauth, appmodel, check, maingen, run


def _config(dev_stub=None, entities=None, providers=None):
    identity = {"providers": providers or []}
    if dev_stub is not None:
        identity["dev_stub"] = dev_stub
    return {
        "project": {"name": "demo"},
        "identity": identity,
        "entities": entities or [{"name": "web", "type": "web_edge"},
                                 {"name": "app", "type": "client"}],
        "connect_points": [{"owner": "web", "consumers": ["app"], "export": "slot ping()"}],
    }


def _findings(config, release=False):
    return check._identity_messages(config, release)


class TheProviderTheFrameworkWrites(unittest.TestCase):
    def test_no_dev_stub_no_provider(self):
        self.assertEqual(appmodel.identity_providers(_config()), [])
        self.assertFalse(appmodel.has_dev_stub(_config()))

    def test_the_shortest_form_is_the_word_true(self):
        config = _config(True)
        self.assertTrue(appmodel.has_dev_stub(config))
        self.assertEqual(appmodel.dev_stub_port(config), appmodel.DEV_STUB_PORT)
        self.assertEqual(appmodel.dev_stub_users(config),
                         [appmodel.DEV_STUB_DEFAULT_USER])

    def test_the_entry_is_loopback_and_carries_no_literal_secret(self):
        provider = appmodel.identity_providers(_config(True))[-1]
        self.assertEqual(provider["name"], appmodel.DEV_STUB_PROVIDER)
        self.assertTrue(provider["dev_stub"])
        for key in ("authorize_url", "token_url", "userinfo_url", "jwks_url", "issuer"):
            self.assertTrue(provider[key].startswith("http://127.0.0.1:"), key)
        # The same rule every other provider follows. A secret is a name here, never a
        # value, even when the value is a stand-in.
        self.assertTrue(provider["client_secret"].startswith("env:"))

    def test_the_project_provider_stays_first(self):
        # The login route reaches for the first provider, and that has to go on meaning
        # the real one after somebody turns the development sign-in on.
        github = {"name": "github", "client_id": "x", "client_secret": "env:S"}
        names = [one["name"] for one in appmodel.identity_providers(
            _config(True, providers=[github]))]
        self.assertEqual(names, ["github", appmodel.DEV_STUB_PROVIDER])

    def test_the_port_moves_the_whole_entry(self):
        provider = appmodel.identity_providers(_config({"port": 9100}))[-1]
        self.assertEqual(appmodel.dev_stub_port(_config({"port": 9100})), 9100)
        self.assertEqual(provider["issuer"], "http://127.0.0.1:9100")
        self.assertEqual(provider["jwks_url"], "http://127.0.0.1:9100/jwks")

    def test_a_port_that_is_not_one_falls_back_rather_than_binding_nothing(self):
        self.assertEqual(appmodel.dev_stub_port(_config({"port": "nine"})),
                         appmodel.DEV_STUB_PORT)
        self.assertEqual(appmodel.dev_stub_port(_config({"port": 0})),
                         appmodel.DEV_STUB_PORT)

    def test_the_entry_passes_the_checks_every_provider_passes(self):
        # http is refused for an identity endpoint everywhere except loopback, and a
        # provider read through an ID token has to name an issuer and a key set. The
        # synthesized entry is held to all of it rather than exempted from any of it.
        self.assertEqual(_findings(_config(True)), [])


class WhoTheSignInOffers(unittest.TestCase):
    def test_configured_people_replace_the_default_one(self):
        users = appmodel.dev_stub_users(_config(
            {"users": [{"sub": "ada", "login": "ada", "email": "ada@localhost"},
                       {"sub": "grace", "login": "grace", "email": "grace@localhost"}]}))
        self.assertEqual([one["sub"] for one in users], ["ada", "grace"])

    def test_a_field_the_identity_object_does_not_have_is_dropped(self):
        # Whatever survives here is what the mapping hook reads, and the hook reads an
        # identity. A `scope:` written beside a dev user would look like it worked.
        users = appmodel.dev_stub_users(_config(
            {"users": [{"sub": "ada", "scope": "admin"}]}))
        self.assertEqual(users, [{"sub": "ada"}])

    def test_an_empty_list_still_leaves_somebody_to_sign_in_as(self):
        self.assertEqual(appmodel.dev_stub_users(_config({"users": []})),
                         [appmodel.DEV_STUB_DEFAULT_USER])


class WhatTheCheckRefuses(unittest.TestCase):
    def test_a_key_nobody_reads(self):
        findings = _findings(_config({"users": [{"sub": "ada"}], "scopes": ["admin"]}))
        self.assertEqual(len(findings), 1)
        self.assertIn("unknown key 'scopes'", findings[0])

    def test_a_user_with_no_sub(self):
        findings = _findings(_config({"users": [{"login": "ada"}]}))
        self.assertEqual(len(findings), 1)
        self.assertIn("has no sub", findings[0])

    def test_a_field_the_identity_object_does_not_have(self):
        findings = _findings(_config({"users": [{"sub": "ada", "scope": "admin"}]}))
        self.assertEqual(len(findings), 1)
        self.assertIn("unknown field 'scope'", findings[0])

    def test_users_that_are_not_a_sequence(self):
        findings = _findings(_config({"users": {"sub": "ada"}}))
        self.assertEqual(len(findings), 1)
        self.assertIn("must be a sequence", findings[0])

    def test_a_port_that_is_not_a_port(self):
        findings = _findings(_config({"port": 70000}))
        self.assertEqual(len(findings), 1)
        self.assertIn("must be a port number", findings[0])

    def test_the_port_an_entity_already_serves_on(self):
        # Both would bind it and one would lose, and the run would end on a message about
        # a port rather than about a login.
        config = _config({"port": 8443},
                         entities=[{"name": "web", "type": "web_edge",
                                    "public": {"host": "127.0.0.1", "port": 8443}},
                                   {"name": "app", "type": "client"}])
        findings = _findings(config)
        self.assertEqual(len(findings), 1)
        self.assertIn("is the port entity 'web' serves on", findings[0])

    def test_a_release_build_is_told_it_carries_one_and_that_it_is_inert(self):
        findings = _findings(_config(True), release=True)
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0].startswith("warn:"))
        self.assertIn("does not run", findings[0])

    def test_nothing_is_said_about_a_project_that_has_none(self):
        self.assertEqual(_findings(_config(), release=True), [])


class WhatTheEdgeIsGenerated(unittest.TestCase):
    def _edge_main(self, config):
        return maingen.render_edge_main(config, config["entities"][0])

    def test_the_server_starts_in_the_edge_and_only_under_dev(self):
        source = self._edge_main(_config(True))
        self.assertIn('#include "stubidentityserver.h"', source)
        started = source.index("StubIdentityServer *devIdentity")
        guard = source.rindex("if (parser.isSet(devOption)) {", 0, started)
        # Nothing between the guard and the construction. The server is inside it.
        self.assertNotIn("}", source[guard:started])
        self.assertIn(f"devIdentity->start({appmodel.DEV_STUB_PORT})", source)

    def test_the_entry_is_marked_so_the_runtime_refuses_it_without_the_flag(self):
        source = self._edge_main(_config(True))
        self.assertIn(".devStub = true;", source)

    def test_the_shared_secret_comes_from_the_environment(self):
        source = self._edge_main(_config(True))
        self.assertIn(f'qEnvironmentVariable("{appmodel.DEV_STUB_SECRET_VARIABLE}")', source)
        # And never from the project file, which is a file in a repository.
        self.assertNotIn("synqt-dev-secret", source)

    def test_every_configured_person_is_offered(self):
        source = self._edge_main(_config(
            {"users": [{"sub": "ada", "login": "ada", "name": "Ada", "email": "a@localhost"},
                       {"sub": "grace", "login": "grace", "name": "Grace",
                        "email": "g@localhost"}]}))
        self.assertIn('devUser0.insert(QStringLiteral("sub"), QStringLiteral("ada"));', source)
        self.assertIn("devIdentity->setUser(devUser0);", source)
        self.assertIn('devUser1.insert(QStringLiteral("sub"), QStringLiteral("grace"));',
                      source)
        self.assertIn("devIdentity->addUser(devUser1);", source)

    def test_a_project_without_one_links_no_http_server_into_its_edge(self):
        source = self._edge_main(_config(
            providers=[{"name": "github", "client_id": "x", "client_secret": "env:S"}]))
        self.assertNotIn("StubIdentityServer", source)
        self.assertNotIn("devStub", source)


class WhatTheScaffoldWrites(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(
            yaml.safe_dump({"project": {"name": "app"}}, sort_keys=False))
        return root

    def test_add_auth_dev_writes_the_block_and_no_provider_to_register(self):
        root = self._project()
        message = addauth.scaffold(root, addauth.DEV_STUB_NAME)

        identity = yaml.safe_load((root / "synqt.yaml").read_text())["identity"]
        self.assertEqual(identity["providers"], [])
        self.assertEqual([one["sub"] for one in identity["dev_stub"]["users"]],
                         ["dev", "mod"])
        # Nothing to register and no secret to place, so the steps are about the two
        # things that are the project's own.
        self.assertNotIn("Register an OAuth app", message)
        self.assertIn("identity.dev_stub.users", message)
        self.assertFalse((root / ".env.example").exists())

    def test_add_auth_for_a_real_provider_is_untouched(self):
        root = self._project()
        addauth.scaffold(root, "github")
        identity = yaml.safe_load((root / "synqt.yaml").read_text())["identity"]
        self.assertEqual(identity["providers"][0]["name"], "github")
        self.assertNotIn("dev_stub", identity)
        self.assertIn("GITHUB_CLIENT_SECRET=", (root / ".env.example").read_text())


class WhatDevHandsTheProcesses(unittest.TestCase):
    """`synqt dev` starts the entities. The environment it hands them is what carries the
    development sign-in's shared secret to both ends of it."""

    def _environments(self, config):
        """The env each launched process was given, with nothing started."""
        launched = []

        class _Popen:
            def __init__(self, command, cwd=None, env=None):
                launched.append(env)

        root = Path(tempfile.mkdtemp())
        original_popen = run.subprocess.Popen
        original_binary = run.host_binary
        run.subprocess.Popen = _Popen
        run.host_binary = lambda _root, name: Path("/nonexistent") / str(name)
        try:
            run._launch_entities(root, config, ["web"], 8080)
        finally:
            run.subprocess.Popen = original_popen
            run.host_binary = original_binary
        return launched

    def test_the_shared_secret_reaches_every_process_and_is_fresh_each_run(self):
        config = _config(True)
        first = self._environments(config)
        second = self._environments(config)

        self.assertEqual(len(first), 1)
        secret = first[0].get(appmodel.DEV_STUB_SECRET_VARIABLE)
        self.assertTrue(secret)
        # Fresh per run, so another process on this machine cannot spend a code against
        # a development token endpoint by knowing what the last run used.
        self.assertNotEqual(secret, second[0].get(appmodel.DEV_STUB_SECRET_VARIABLE))

    def test_a_project_with_no_dev_sign_in_is_handed_nothing(self):
        environments = self._environments(_config())
        self.assertEqual(len(environments), 1)
        self.assertNotIn(appmodel.DEV_STUB_SECRET_VARIABLE, environments[0])


class WhatDevSays(unittest.TestCase):
    def test_the_sign_in_is_named_because_the_page_does_not_show_it(self):
        summary = run.dev_summary(
            _config({"users": [{"sub": "ada", "login": "ada"},
                               {"sub": "grace", "login": "grace"}]}),
            "http://127.0.0.1:8080/", ["db", "web"])
        self.assertIn("Development sign-in on port 8789: ada, grace", summary)
        self.assertIn("Not in a build", summary)

    def test_a_project_with_none_is_told_nothing_about_one(self):
        summary = run.dev_summary(_config(), "http://127.0.0.1:8080/", ["web"])
        self.assertNotIn("sign-in", summary)


class WhatARealProviderIsAskedForAndADevelopmentOneIsNot(unittest.TestCase):
    def test_a_release_build_is_not_told_about_a_redirect_uri_nobody_will_use(self):
        # The origin advice is about the `redirect_uri` a provider compares. A project
        # whose only sign-in is the development one has no provider to compare anything,
        # and that sign-in is refused in a build anyway.
        config = _config(True)
        self.assertEqual(check._derived_origin_messages(config, True), [])

    def test_a_real_provider_with_no_origin_is_still_told(self):
        config = _config(True, providers=[{"name": "github", "client_id": "x",
                                           "client_secret": "env:S"}])
        findings = check._derived_origin_messages(config, True)
        self.assertEqual(len(findings), 1)
        self.assertIn("declares no public.origin", findings[0])


if __name__ == "__main__":
    unittest.main()
