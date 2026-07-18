# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add auth <provider>``: scaffold login with secure defaults.

The command writes the ``identity`` section and a provider entry with the hardened
defaults from ``docs/authentication.md``, records the client secret as an ``env:``
reference (never a literal), adds a ``.env.example`` entry, scaffolds the identity
mapping hook, and prints only the manual steps the developer must still do. Everything
that makes login safe (PKCE, the framework-generated state, the httpOnly/Secure/SameSite
cookie, edge-held tokens, ID-token verification, rotation, the origin and upgrade checks)
is on by default and is not expressed here because it does not depend on configuration.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

import yaml


class AddAuthError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _secret_env(provider: str) -> str:
    return provider.upper().replace("-", "_") + "_CLIENT_SECRET"


def provider_template(provider: str) -> Dict[str, Any]:
    """The provider entry, mapping raw provider fields to the normalized identity.

    Each template documents which endpoints and scopes it needs. The client secret is
    always an ``env:`` reference; the real value lives only in the edge ``.env``.
    """
    secret = f"env:{_secret_env(provider)}"
    if provider == "github":
        # Plain OAuth2: identity from /user, with the numeric id mapped to sub and the
        # primary verified address pulled from /user/emails when a private email hides it.
        return {
            "name": "github",
            "authorize_url": "https://github.com/login/oauth/authorize",
            "token_url": "https://github.com/login/oauth/access_token",
            "userinfo_url": "https://api.github.com/user",
            "emails_url": "https://api.github.com/user/emails",
            "scopes": ["read:user", "user:email"],
            "client_id": "your-github-client-id",
            "client_secret": secret,
            "sub_field": "id",
        }
    if provider == "google":
        # OpenID Connect: identity from the JWKS-verified ID token.
        return {
            "name": "google",
            "authorize_url": "https://accounts.google.com/o/oauth2/v2/auth",
            "token_url": "https://oauth2.googleapis.com/token",
            "jwks_url": "https://www.googleapis.com/oauth2/v3/certs",
            "issuer": "https://accounts.google.com",
            "use_id_token": True,
            "scopes": ["openid", "email", "profile"],
            "client_id": "your-google-client-id",
            "client_secret": secret,
        }
    # A generic OpenID Connect provider, to be pointed at any compliant issuer.
    return {
        "name": provider,
        "authorize_url": f"https://{provider}.example/authorize",
        "token_url": f"https://{provider}.example/token",
        "jwks_url": f"https://{provider}.example/.well-known/jwks.json",
        "issuer": f"https://{provider}.example",
        "use_id_token": True,
        "scopes": ["openid", "email", "profile"],
        "client_id": f"your-{provider}-client-id",
        "client_secret": secret,
    }


def identity_section(provider: str, required: bool, provider_entity: str) -> Dict[str, Any]:
    """The full ``identity`` section, hardened by default."""
    return {
        "required": required,
        "provider_entity": provider_entity,
        "flow": "authorization_code",  # server-side Authorization Code + PKCE
        "callback": "/auth/callback",
        "login": "/auth/login",
        "logout": "/auth/logout",
        "providers": [provider_template(provider)],
        "session": {
            "cookie_name": "synqt_session",
            "same_site": "lax",  # lax for same_origin; the framework sets httpOnly + Secure
            "ttl_minutes": 720,
            "rotate": True,  # rotate the session id on privilege change
        },
        "mapping": {"hook": "web/identity/map.qml"},
    }


MAP_HOOK = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// Turn a normalized identity into a SynQt scope, on the edge, after a successful login.
// Tolerate a null email: prefer sub or login for authorization decisions.
IdentityMapping {
    function scopeFor(identity) {
        const admins = [];       // e.g. "you@example.com"
        const moderators = [];
        if (admins.indexOf(identity.email) !== -1) {
            return "admin";
        }
        if (moderators.indexOf(identity.email) !== -1) {
            return "moderator";
        }
        return "user";           // any successfully authenticated user
    }
}
"""


def manual_steps(provider: str, provider_entity: str = "") -> str:
    secret = _secret_env(provider)
    # Where the client secret lives depends on where identity runs. In process it is the
    # edge; with provider_entity the OAuth engine (token exchange + secret + tokens) runs on
    # the auth entity, so the secret belongs in the auth entity's .env, never the edge's.
    if provider_entity:
        secret_step = (
            f"  3. Put the client secret in the '{provider_entity}' auth entity's .env as "
            f"{secret} (never in synqt.yaml, never on the edge, never in a client target).\n"
        )
    else:
        secret_step = (
            f"  3. Put the client secret in the edge .env as {secret} "
            "(never in synqt.yaml, never in a client target).\n"
        )
    return (
        f"Auth scaffolded for '{provider}'. Do only these; everything else is already "
        "secure by default:\n"
        f"  1. Register an OAuth app with {provider}.\n"
        "  2. Set its redirect URL to your edge callback: <edge-origin>/auth/callback\n"
        + secret_step
        + "  4. Edit web/identity/map.qml to grant higher scopes to specific identities."
    )


def scaffold(project_dir: os.PathLike[str] | str, provider: str, *, required: bool = False,
             provider_entity: str = "") -> str:
    """Apply the scaffolding under ``project_dir`` and return the manual-steps message.

    Refuses to clobber an existing ``identity`` section.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        loaded = yaml.safe_load(config_path.read_text()) or {}
        if not isinstance(loaded, dict):
            raise AddAuthError("synqt.yaml is not a mapping")
        config = loaded
    if "identity" in config:
        raise AddAuthError(
            "an 'identity' section already exists; edit it by hand rather than re-running "
            "'synqt add auth'")

    config["identity"] = identity_section(provider, required, provider_entity)
    config_path.write_text(yaml.safe_dump(config, sort_keys=False))

    # Document the required secret as unset, so it is discoverable but never committed.
    env_example = root / ".env.example"
    secret = _secret_env(provider)
    lines: List[str] = []
    if env_example.exists():
        lines = env_example.read_text().splitlines()
    if not any(line.startswith(secret + "=") for line in lines):
        lines.append(f"{secret}=")
        env_example.write_text("\n".join(lines) + "\n")

    # Scaffold the mapping hook (never overwrite an edited one).
    hook = root / "web" / "identity" / "map.qml"
    if not hook.exists():
        hook.parent.mkdir(parents=True, exist_ok=True)
        hook.write_text(MAP_HOOK)

    return manual_steps(provider, provider_entity)
