# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt mesh``: the project certificate authority and per-entity certificates.

Service entities authenticate each other with mutual TLS against a private project CA
(docs/build-system-and-cli.md):

- The CA private key is created once in ``synqt/mesh/`` with restrictive permissions,
  git-ignored, and only issues certificates. It is never copied into a running entity, which
  holds its own certificate and key plus ``ca.crt``.
- Each entity certificate carries the entity name as its subject.
- The client entity gets no mesh certificate.
"""

from __future__ import annotations

import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional

from . import devidentities
from .appmodel import ENTITY_NAME_MAX, is_valid_entity_name

# Leaf validity: 398 days, the CA/Browser Forum maximum. Apple's verifier rejects a TLS leaf
# issued after 2020-09-01 valid for longer. The CA gets twice this, so rotating leaves does
# not require a new anchor (see `synqt mesh rotate`).
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


def _reserve_key(path: Path) -> None:
    """Create the file a private key is about to be written into, readable only by this user.

    ``openssl genrsa -out`` creates the file with the umask, and a later chmod leaves a
    window. The file is created first with its mode, and openssl writes into it without
    resetting the mode. On Windows ``restrict`` applies the ACL afterwards.
    """
    path.unlink(missing_ok=True)
    if os.name == "nt":
        return
    os.close(os.open(path, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600))


def restrict(path: Path) -> bool:
    """Make a private key readable only by its owner. Returns whether the platform mechanism
    was applied: on Windows os.chmod only toggles the read-only bit, so the caller reports
    it.
    """
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


def ensure_gitignored(project_dir: os.PathLike[str] | str) -> None:
    # Never commit the CA key, entity keys, or `.dev-identities` (one developer's list of
    # people).
    gitignore = Path(project_dir) / ".gitignore"
    rules = ["synqt/mesh/*.key", "synqt/mesh/dev/", "synqt/toolchain/",
             devidentities.FILE_NAME]
    existing = gitignore.read_text(encoding="utf-8").splitlines() if gitignore.exists() else []
    added = [rule for rule in rules if rule not in existing]
    if added:
        with gitignore.open("a") as handle:
            if existing and existing[-1].strip():
                handle.write("\n")
            handle.write("# SynQt: never commit mesh private keys, the toolchain cache, or the\n"
                         "# development identities of whoever works on this machine\n")
            handle.write("\n".join(added) + "\n")


def init(project_dir: os.PathLike[str] | str, *, dev: bool = False, force: bool = False) -> str:
    """Create the project private CA. The CA key is written 0600 and git-ignored."""
    mesh = _mesh_dir(project_dir, dev)
    mesh.mkdir(parents=True, exist_ok=True)
    ca_key = mesh / "ca.key"
    ca_crt = mesh / "ca.crt"
    if ca_key.exists() and not force:
        raise MeshError(f"a CA already exists at {ca_key}; use rotate or --force")

    # The CA extensions are stated explicitly, not inherited from openssl.cnf. `req -x509
    # -addext` appends to the config section, and LibreSSL (macOS) then emits
    # basicConstraints twice, which RFC 5280 4.2 forbids and Apple's verifier rejects.
    # Signing a CSR with `x509 -req -extfile`, as for entity certificates, makes the
    # extension file the only source. keyUsage is stated so strict verifiers build chains
    # through the CA.
    csr = mesh / "ca.csr"
    ext = mesh / "ca.ext"
    ext.write_text(
        "basicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid:always\n", encoding="utf-8")
    try:
        _reserve_key(ca_key)
        _openssl("genrsa", "-out", str(ca_key), "2048")
        _openssl("req", "-new", "-key", str(ca_key), "-subj", "/CN=SynQt Mesh CA",
                 "-out", str(csr))
        _openssl("x509", "-req", "-in", str(csr), "-signkey", str(ca_key), "-sha256",
                 "-days", str(VALIDITY_DAYS * 2), "-extfile", str(ext), "-out", str(ca_crt))
    finally:
        ext.unlink(missing_ok=True)
        csr.unlink(missing_ok=True)
    restricted = restrict(ca_key)
    ensure_gitignored(project_dir)
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
    # The name comes from a prompt and goes into file names, the subject `/CN=` and the SAN,
    # so it is held to the entity name rule `synqt check` applies.
    if not is_valid_entity_name(entity):
        raise MeshError(
            f"'{entity[:80]}' is not usable as an entity name: a name starts with a letter "
            f"and is made of letters, digits, underscores and hyphens, up to "
            f"{ENTITY_NAME_MAX} characters")
    mesh = _mesh_dir(project_dir, dev)
    ca_key = mesh / "ca.key"
    ca_crt = mesh / "ca.crt"
    if not ca_key.exists():
        raise MeshError(f"no CA at {mesh}; run 'synqt mesh init' first")

    key = mesh / f"{entity}.key"
    csr = mesh / f"{entity}.csr"
    crt = mesh / f"{entity}.crt"
    # The extensions go through a real file: Windows has no /dev/stdin. An entity is a TLS
    # server on the links it owns and a client on those it consumes, so it needs serverAuth
    # and clientAuth. Apple's verifier requires the usage OID; OpenSSL accepts a certificate
    # with no EKU.
    ext = mesh / f"{entity}.ext"
    ext.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth,clientAuth\n"
        f"subjectAltName=DNS:{entity}\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n", encoding="utf-8")
    try:
        _reserve_key(key)
        _openssl("genrsa", "-out", str(key), "2048")
        _openssl("req", "-new", "-key", str(key), "-subj", f"/CN={entity}", "-out", str(csr))
        _openssl("x509", "-req", "-in", str(csr), "-CA", str(ca_crt), "-CAkey", str(ca_key),
                 "-CAcreateserial", "-days", str(VALIDITY_DAYS), "-sha256",
                 "-extfile", str(ext), "-out", str(crt))
    finally:
        ext.unlink(missing_ok=True)
        csr.unlink(missing_ok=True)
    restrict(key)
    ensure_gitignored(project_dir)
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


#: The English month names openssl prints. `strptime` `%b` follows the locale, so they are
#: parsed by hand.
_MONTHS = {name: number for number, name in enumerate(
    ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"], start=1)}


def _not_after(crt: Path) -> Optional[datetime]:
    output = _openssl("x509", "-enddate", "-noout", "-in", str(crt)).strip()
    if not output.startswith("notAfter="):
        return None
    # "Aug  4 12:00:00 2027 GMT": the day is space-padded, so split on runs of whitespace.
    fields = output[len("notAfter="):].split()
    if len(fields) < 4 or fields[0] not in _MONTHS:
        return None
    try:
        clock = datetime.strptime(fields[2], "%H:%M:%S")
        return datetime(int(fields[3]), _MONTHS[fields[0]], int(fields[1]),
                        clock.hour, clock.minute, clock.second, tzinfo=timezone.utc)
    except ValueError:
        return None


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
        # Expired first: an expired certificate also falls within warn_days.
        if days < 0:
            flag = "  <-- EXPIRED"
        elif days <= warn_days:
            flag = "  <-- EXPIRES SOON"
        else:
            flag = ""
        lines.append(f"  {crt.stem}: valid until {expiry:%Y-%m-%d} ({days} days){flag}")
    return "\n".join(lines)
