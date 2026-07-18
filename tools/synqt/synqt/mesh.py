# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt mesh``: the project certificate authority and per-entity certificates.

Service entities authenticate each other with mutual TLS against a project private CA.
This module manages the CA and the entity certificates so a developer never runs raw
openssl. The rules it enforces (see docs/build-system-and-cli.md):

- The CA private key is created once, kept in ``synqt/mesh/`` with restrictive
  permissions, git-ignored, and used only to issue certs. It is NEVER copied into a
  running entity (a running entity holds only its own cert+key plus ``ca.crt``).
- Each entity certificate carries the entity name as its subject, so a verified peer
  certificate tells an owner which entity is calling.
- The client entity gets no mesh certificate; it authenticates with a user session.
"""

from __future__ import annotations

import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional

# ~13 months. This was 825 days, described as the CA/Browser Forum leaf maximum, which it
# stopped being in September 2020 when that ceiling dropped to 398. The number is not
# cosmetic: Apple's verifier rejects a TLS leaf issued after 2020-09-01 whose validity runs
# past 398 days, whatever it chains to, so an entity certificate issued at the old maximum is
# one a macOS host can refuse on sight. The CA is issued for twice this, so rotating leaves
# does not silently need a new anchor too (see `synqt mesh rotate`).
VALIDITY_DAYS = 398


class MeshError(Exception):
    """A mesh-tooling error surfaced to the CLI (no traceback for the user)."""


def _openssl(*args: str) -> str:
    try:
        result = subprocess.run(
            ["openssl", *args], capture_output=True, text=True, check=True)
    except FileNotFoundError as error:
        raise MeshError("openssl is not installed or not on PATH") from error
    except subprocess.CalledProcessError as error:
        raise MeshError(f"openssl {args[0]} failed: {error.stderr.strip()}") from error
    return result.stdout


def _restrict(path: Path) -> bool:
    """Make a private key readable only by the user who owns it. Returns whether the
    platform's own mechanism was applied, because the caller reports it: os.chmod on
    Windows only toggles the read-only bit (it leaves the file world-readable, and
    stat reports 0666 whatever mode is passed), so a bare chmod there would let the
    tool claim a protection it had not applied to the one key that matters most."""
    if os.name != "nt":
        os.chmod(path, 0o600)
        return True
    user = os.environ.get("USERNAME")
    if not user:
        return False
    try:
        # Drop inherited ACEs, then grant this user alone full control.
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", f"{user}:F"],
                       capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def _mesh_dir(project_dir: os.PathLike[str] | str, dev: bool = False) -> Path:
    root = Path(project_dir) / "synqt" / "mesh"
    return root / "dev" if dev else root


def _ensure_gitignored(project_dir: os.PathLike[str] | str) -> None:
    # The CA key and entity keys must never be committed.
    gitignore = Path(project_dir) / ".gitignore"
    rules = ["synqt/mesh/*.key", "synqt/mesh/dev/", "synqt/toolchain/"]
    existing = gitignore.read_text().splitlines() if gitignore.exists() else []
    added = [rule for rule in rules if rule not in existing]
    if added:
        with gitignore.open("a") as handle:
            if existing and existing[-1].strip():
                handle.write("\n")
            handle.write("# SynQt: never commit mesh private keys or the toolchain cache\n")
            handle.write("\n".join(added) + "\n")


def init(project_dir: os.PathLike[str] | str, *, dev: bool = False, force: bool = False) -> str:
    """Create the project private CA. The CA key is written 0600 and git-ignored."""
    mesh = _mesh_dir(project_dir, dev)
    mesh.mkdir(parents=True, exist_ok=True)
    ca_key = mesh / "ca.key"
    ca_crt = mesh / "ca.crt"
    if ca_key.exists() and not force:
        raise MeshError(f"a CA already exists at {ca_key}; use rotate or --force")

    # State the CA's extensions rather than inheriting whatever openssl.cnf happens to
    # default to: this is the trust anchor for every mesh link, so what it is allowed to do
    # is part of the tool's contract, not of the host's configuration. That was the intent
    # of the `req -x509 -addext` this replaces, and -addext does not deliver it: it appends
    # to the config's x509_extensions section instead of replacing it, so the anchor came
    # out carrying that section's basicConstraints *and* ours. OpenSSL 3 collapses the pair;
    # LibreSSL, which is the `openssl` on every macOS machine, emits both, and a repeated
    # extension is invalid per RFC 5280 4.2 -- Apple's verifier duly refuses the anchor and
    # every mesh link on that host fails to verify. Signing a CSR with `x509 -req -extfile`
    # is the same route the entity certs below take: the extension file is the only source
    # of extensions, so the profile is identical on every host's openssl.
    #
    # keyUsage is the addition over the default v3_ca profile -- a CA whose key usage is
    # unstated is one a strict verifier may decline to build a chain through.
    csr = mesh / "ca.csr"
    ext = mesh / "ca.ext"
    ext.write_text(
        "basicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid:always\n")
    try:
        _openssl("genrsa", "-out", str(ca_key), "2048")
        _openssl("req", "-new", "-key", str(ca_key), "-subj", "/CN=SynQt Mesh CA",
                 "-out", str(csr))
        _openssl("x509", "-req", "-in", str(csr), "-signkey", str(ca_key), "-sha256",
                 "-days", str(VALIDITY_DAYS * 2), "-extfile", str(ext), "-out", str(ca_crt))
    finally:
        ext.unlink(missing_ok=True)
        csr.unlink(missing_ok=True)
    restricted = _restrict(ca_key)
    _ensure_gitignored(project_dir)
    protection = ("ca.key is restricted to you and git-ignored" if restricted else
                  "ca.key is git-ignored, but this platform's permissions could NOT be "
                  "restricted: protect it yourself")
    return f"Created the {'dev ' if dev else ''}mesh CA at {mesh} ({protection})."


def cert(project_dir: os.PathLike[str] | str, entity: str, *, dev: bool = False,
         kind: str = "service") -> str:
    """Issue a certificate + key for one entity (subject = entity name)."""
    if kind == "client":
        raise MeshError(
            f"'{entity}' is a client entity: the client gets no mesh certificate "
            "(it authenticates to the edge with a user session, not mutual TLS)")
    mesh = _mesh_dir(project_dir, dev)
    ca_key = mesh / "ca.key"
    ca_crt = mesh / "ca.crt"
    if not ca_key.exists():
        raise MeshError(f"no CA at {mesh}; run 'synqt mesh init' first")

    key = mesh / f"{entity}.key"
    csr = mesh / f"{entity}.csr"
    crt = mesh / f"{entity}.crt"
    # The extensions go through a real file, not /dev/stdin: openssl opens -extfile by
    # name, and Windows has no such device, so feeding the extension in on stdin works on a
    # developer's Linux box and makes `synqt mesh cert` fail outright there.
    #
    # Both key usages are deliberate. A mesh entity is a TLS server on the links it owns
    # and a TLS client on the links it consumes -- the same certificate authenticates it
    # in both directions -- so it needs serverAuth and clientAuth. Naming them (rather
    # than omitting extendedKeyUsage entirely) is what keeps the certificate portable:
    # OpenSSL only enforces an EKU that is present, so an EKU-less cert passes on Linux,
    # while Apple's verifier requires TLS certificates to carry the matching usage OID.
    # A cert without one is not "unrestricted" everywhere, it is untrusted on macOS.
    ext = mesh / f"{entity}.ext"
    ext.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth,clientAuth\n"
        f"subjectAltName=DNS:{entity}\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n")
    try:
        _openssl("genrsa", "-out", str(key), "2048")
        _openssl("req", "-new", "-key", str(key), "-subj", f"/CN={entity}", "-out", str(csr))
        _openssl("x509", "-req", "-in", str(csr), "-CA", str(ca_crt), "-CAkey", str(ca_key),
                 "-CAcreateserial", "-days", str(VALIDITY_DAYS), "-sha256",
                 "-extfile", str(ext), "-out", str(crt))
    finally:
        ext.unlink(missing_ok=True)
        csr.unlink(missing_ok=True)
    _restrict(key)
    _ensure_gitignored(project_dir)
    return f"Issued {entity}.crt (subject CN={entity}, SAN DNS:{entity})."


def cert_all(project_dir: os.PathLike[str] | str, service_entities: List[str],
             *, dev: bool = False) -> str:
    issued = [cert(project_dir, name, dev=dev) for name in service_entities]
    return "\n".join(issued) if issued else "No service entities in the topology."


def rotate(project_dir: os.PathLike[str] | str, entity: Optional[str] = None,
           service_entities: Optional[List[str]] = None, *, dev: bool = False) -> str:
    if entity:
        return cert(project_dir, entity, dev=dev)
    return cert_all(project_dir, service_entities or [], dev=dev)


def _not_after(crt: Path) -> Optional[datetime]:
    output = _openssl("x509", "-enddate", "-noout", "-in", str(crt)).strip()
    if not output.startswith("notAfter="):
        return None
    return datetime.strptime(output[len("notAfter="):], "%b %d %H:%M:%S %Y %Z").replace(
        tzinfo=timezone.utc)


def status(project_dir: os.PathLike[str] | str, *, dev: bool = False,
           warn_days: int = 30) -> str:
    mesh = _mesh_dir(project_dir, dev)
    if not (mesh / "ca.crt").exists():
        return f"No CA at {mesh}. Run 'synqt mesh init'."
    lines = [f"Certificates in {mesh}:"]
    now = datetime.now(timezone.utc)
    for crt in sorted(mesh.glob("*.crt")):
        expiry = _not_after(crt)
        if expiry is None:
            lines.append(f"  {crt.stem}: unreadable")
            continue
        days = (expiry - now).days
        flag = "  <-- EXPIRES SOON" if days <= warn_days else ("  <-- EXPIRED" if days < 0 else "")
        lines.append(f"  {crt.stem}: valid until {expiry:%Y-%m-%d} ({days} days){flag}")
    return "\n".join(lines)
