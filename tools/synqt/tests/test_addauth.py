# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add auth` produces a configuration that is secure with no manual hardening."""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import addauth


class AddAuthTest(unittest.TestCase):
    def _fresh_project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(
            yaml.safe_dump({"project": {"name": "app", "version": "0.1.0"}}, sort_keys=False))
        return root

    def test_github_scaffold_is_secure_by_default(self):
        root = self._fresh_project()
        message = addauth.scaffold(root, "github")

        config = yaml.safe_load((root / "synqt.yaml").read_text())
        identity = config["identity"]

        # Server-side Authorization Code (PKCE and state are framework defaults, not config).
        self.assertEqual(identity["flow"], "authorization_code")
        self.assertFalse(identity["required"])
        self.assertEqual(identity["provider_entity"], "")

        provider = identity["providers"][0]
        self.assertEqual(provider["name"], "github")
        # The secret is an env reference, never a literal in the committed config.
        self.assertEqual(provider["client_secret"], "env:GITHUB_CLIENT_SECRET")
        self.assertIn("user:email", provider["scopes"])
        self.assertEqual(provider["sub_field"], "id")  # numeric id -> stable sub

        session = identity["session"]
        self.assertEqual(session["cookie_name"], "synqt_session")
        self.assertEqual(session["same_site"], "lax")
        self.assertTrue(session["rotate"])
        self.assertEqual(identity["mapping"]["hook"], "web/identity/map.qml")

        # No plaintext secret is written anywhere in the config.
        self.assertNotIn("client_secret: '", (root / "synqt.yaml").read_text())
        raw = (root / "synqt.yaml").read_text()
        self.assertIn("env:GITHUB_CLIENT_SECRET", raw)

        # The required secret is documented but unset, and the mapping hook is scaffolded.
        self.assertIn("GITHUB_CLIENT_SECRET=", (root / ".env.example").read_text())
        hook = (root / "web" / "identity" / "map.qml").read_text()
        self.assertIn("IdentityMapping", hook)
        self.assertIn("scopeFor", hook)

        # The printed message says only what the developer must still do.
        self.assertIn("redirect URL", message)
        self.assertIn("GITHUB_CLIENT_SECRET", message)
        self.assertIn("secure by default", message)
        self.assertNotIn("add the", message.lower())  # no "remember to add" hardening steps

    def test_required_flag(self):
        root = self._fresh_project()
        addauth.scaffold(root, "github", required=True)
        identity = yaml.safe_load((root / "synqt.yaml").read_text())["identity"]
        self.assertTrue(identity["required"])

    def test_oidc_provider_uses_id_token_and_jwks(self):
        root = self._fresh_project()
        addauth.scaffold(root, "google")
        provider = yaml.safe_load((root / "synqt.yaml").read_text())["identity"]["providers"][0]
        self.assertTrue(provider["use_id_token"])
        self.assertIn("jwks_url", provider)
        self.assertEqual(provider["issuer"], "https://accounts.google.com")
        self.assertIn("openid", provider["scopes"])
        self.assertEqual(provider["client_secret"], "env:GOOGLE_CLIENT_SECRET")

    def test_provider_entity(self):
        root = self._fresh_project()
        message = addauth.scaffold(root, "github", provider_entity="auth")
        identity = yaml.safe_load((root / "synqt.yaml").read_text())["identity"]
        self.assertEqual(identity["provider_entity"], "auth")
        # Centralized: the secret lives on the auth entity (where the OAuth engine runs),
        # never on the edge. The manual steps must say so.
        self.assertIn("'auth' auth entity's .env", message)
        self.assertIn("never on the edge", message)

    def test_refuses_to_clobber_existing_identity(self):
        root = self._fresh_project()
        addauth.scaffold(root, "github")
        with self.assertRaises(addauth.AddAuthError):
            addauth.scaffold(root, "github")


if __name__ == "__main__":
    unittest.main()
