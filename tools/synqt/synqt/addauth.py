# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add auth <provider>``: scaffold login with secure defaults.

Writes the ``identity`` section and a provider entry with the defaults from
``docs/authentication.md``, the client secret as an ``env:`` reference, a ``.env.example``
entry and the mapping hook, and prints the manual steps left. PKCE, the framework state, the
cookie attributes, edge-held tokens, ID-token verification, rotation and the origin and
upgrade checks are always on and need no configuration.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List

# PyYAML is imported inside scaffold() only. appmodel reads this module's provider table,
# and synqt.clientshell (run by tools/wasm-shell.py with no install) imports appmodel, so a
# module-level import would make PyYAML required there.


#: A provider name: it becomes `<NAME>_CLIENT_SECRET` and the host of a template's URLs.
_PROVIDER_NAME = re.compile(r"^[a-z][a-z0-9-]*$")


class AddAuthError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _secret_env(provider: str) -> str:
    return provider.upper().replace("-", "_") + "_CLIENT_SECRET"


# The providers known by name, whose templates carry real endpoints. Only these are filled
# in under a short form by :func:`synqt.appmodel.identity_providers`; the generic OpenID
# Connect entry has placeholder endpoints.
TEMPLATED_PROVIDERS = ("github", "google")

#: The name for the development sign-in: it writes `identity.dev_stub` and no provider
#: entry. The framework writes that entry.
DEV_STUB_NAME = "dev"


def provider_template(provider: str) -> Dict[str, Any]:
    """The provider entry, mapping raw provider fields to the normalized identity. The client
    secret is always an ``env:`` reference (`env:GITHUB_CLIENT_SECRET`); the value lives in
    the edge ``.env``.
    """
    secret_ref = f"env:{_secret_env(provider)}"
    if provider == "github":
        # Plain OAuth2: identity from /user, the numeric id as sub, and the primary verified
        # address from /user/emails when the email is private.
        return {
            "name": "github",
            "authorize_url": "https://github.com/login/oauth/authorize",
            "token_url": "https://github.com/login/oauth/access_token",
            "userinfo_url": "https://api.github.com/user",
            "emails_url": "https://api.github.com/user/emails",
            "scopes": ["read:user", "user:email"],
            "client_id": "your-github-client-id",
            "client_secret": secret_ref,
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
            "client_secret": secret_ref,
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
        "client_secret": secret_ref,
    }


#: The mapping hook path when no web edge is declared yet: the `edge` that `synqt new`
#: creates.
DEFAULT_HOOK = "web/edge/identity/map.qml"

#: The scopes `synqt add auth` writes for a project that declares none: the ones MAP_HOOK
#: names, lowest authority first. `synqt new` writes the same four.
SCAFFOLD_SCOPES = ("anonymous", "user", "moderator", "admin")


def hook_path(config: Dict[str, Any]) -> str:
    """Where this project's mapping hook belongs: inside its edge folder."""
    # Local import: appmodel imports this module for its provider table.
    from . import appmodel

    for entity in appmodel.entities(config):
        if appmodel.is_edge(entity) and entity.get("name"):
            return f"{appmodel.entity_dir(entity)}/identity/map.qml"
    return DEFAULT_HOOK


def identity_section(provider: str, required: bool, provider_entity: str,
                     hook: str = DEFAULT_HOOK) -> Dict[str, Any]:
    """The full ``identity`` section, hardened by default."""
    development = provider == DEV_STUB_NAME
    section: Dict[str, Any] = {
        "required": required,
        "provider_entity": provider_entity,
        "flow": "authorization_code",  # server-side Authorization Code + PKCE
        "callback": "/auth/callback",
        "login": "/auth/login",
        "logout": "/auth/logout",
        "providers": [] if development else [provider_template(provider)],
        # SameSite follows project.origin_model, and the cookie is always httpOnly and
        # Secure; the session id is replaced on every change of scope.
        "session": {
            "cookie_name": "synqt_session",
            "ttl_minutes": 720,
        },
        "mapping": {"hook": hook},
    }
    if development:
        # Two people, to exercise more than one scope through the mapping hook.
        section["dev_stub"] = {
            "users": [{"sub": "dev", "login": "dev", "name": "Developer",
                       "email": "dev@localhost"},
                      {"sub": "mod", "login": "mod", "name": "Moderator",
                       "email": "moderator@localhost"}],
        }
    return section


MAP_HOOK = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// Turn a normalized identity into a SynQt scope, on the edge, after a successful login.
// The email is null unless the provider verified the address, so tolerate a null one.
// Prefer sub or login for authorization decisions.
//
// Return a member of Scope, which SynQt generates next to this file from scopes.order in
// synqt.yaml, so only declared scopes can be named. The edge reads the answer as an index
// into that list and refuses the login when it is out of range.
IdentityMapping {
    function scopeFor(identity): int {
        const admins = [];       // e.g. "you@example.com"
        const moderators = [];
        if (admins.indexOf(identity.email) !== -1) {
            return Scope.Admin;
        }
        if (moderators.indexOf(identity.email) !== -1) {
            return Scope.Moderator;
        }
        return Scope.User; // any successfully authenticated user
    }
}
"""


def manual_steps(provider: str, provider_entity: str = "",
                 hook: str = DEFAULT_HOOK) -> str:
    if provider == DEV_STUB_NAME:
        # No app to register and no secret to place: only the users and the hook are left.
        return (
            "The development sign-in is configured. It runs under 'synqt dev' only: "
            "'synqt serve' passes no flag, and a shipped edge refuses the provider even "
            "if it had one.\n"
            "  1. Edit identity.dev_stub.users in synqt.yaml to be the people your app "
            "cares about.\n"
            f"  2. Edit {hook} to give each of them a scope; that hook is the one a real "
            "provider's identity goes through too.\n"
            "  3. Run 'synqt add auth <provider>' when you want the real thing; the "
            "development sign-in can stay beside it.")
    # `secret_env` is the name of the variable to set.
    secret_env = _secret_env(provider)
    # In process the secret belongs to the edge. With provider_entity it belongs in the auth
    # entity .env, never the edge's.
    if provider_entity:
        secret_step = (
            f"  3. Put the client secret in the '{provider_entity}' auth entity's .env as "
            f"{secret_env} (never in synqt.yaml, never on the edge, never in a client "
            "target).\n"
        )
    else:
        secret_step = (
            f"  3. Put the client secret in the edge .env as {secret_env} "
            "(never in synqt.yaml, never in a client target).\n"
        )
    return (
        f"Auth scaffolded for '{provider}'. Do only these; everything else is already "
        "secure by default:\n"
        f"  1. Register an OAuth app with {provider}.\n"
        "  2. Set its redirect URL to your edge callback: <edge-origin>/auth/callback\n"
        + secret_step
        + f"  4. Edit {hook} to grant higher scopes to specific identities."
    )


def scaffold(project_dir: os.PathLike[str] | str, provider: str, *, required: bool = False,
             provider_entity: str = "") -> str:
    """Apply the scaffolding under ``project_dir`` and return the manual-steps message. Refuses
    to overwrite an existing ``identity`` section.
    """
    # Local imports: yamledit imports PyYAML too (see the note at the top).
    import yaml
    from synqt import yamledit

    if not _PROVIDER_NAME.match(provider or ""):
        raise AddAuthError(
            f"'{provider[:80]}' cannot name a provider: it becomes the variable "
            f"<NAME>_CLIENT_SECRET and part of its URLs, so it is lower case letters, digits "
            "and hyphens, starting with a letter (github, google, my-idp)")

    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        loaded = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}
        if not isinstance(loaded, dict):
            raise AddAuthError("synqt.yaml is not a mapping")
        config = loaded
    if "identity" in config:
        raise AddAuthError(
            "an 'identity' section already exists; edit it by hand rather than re-running "
            "'synqt add auth'")

    # Spliced into the text, keeping the author's comments and formatting.
    hook_relative = hook_path(config)
    section = identity_section(provider, required, provider_entity, hook_relative)
    existing = config_path.read_text(encoding="utf-8") if config_path.exists() else ""
    text = yamledit.set_scalar(existing, "identity", section)

    # A project that signs people in must declare its scopes. These four are the ones the
    # hook names, in authority order (the generated Scope enum values). An existing
    # vocabulary is kept.
    if not isinstance(config.get("scopes"), dict):
        text = yamledit.set_scalar(text, "scopes", {"order": list(SCAFFOLD_SCOPES),
                                                    "hierarchical": True,
                                                    "default": SCAFFOLD_SCOPES[0]})
    config_path.write_text(text, encoding="utf-8")

    # Document the variable with no value (`GITHUB_CLIENT_SECRET=`). The development sign-in
    # needs none: `synqt dev` mints its secret per run.
    if provider != DEV_STUB_NAME:
        env_example = root / ".env.example"
        secret_env = _secret_env(provider)
        lines: List[str] = []
        if env_example.exists():
            lines = env_example.read_text(encoding="utf-8").splitlines()
        if not any(line.startswith(secret_env + "=") for line in lines):
            lines.append(f"{secret_env}=")
            env_example.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # Scaffold the mapping hook (never overwrite an edited one).
    hook = root / hook_relative
    if not hook.exists():
        hook.parent.mkdir(parents=True, exist_ok=True)
        hook.write_text(MAP_HOOK, encoding="utf-8")

    return manual_steps(provider, provider_entity, hook_relative)
