# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What applying a design document would do to a project, worked out before any of it runs.

The editor may do anything a person can do to a topology, deletions included, so what keeps
it safe is not a short list of permitted verbs: it is that nothing happens until somebody has
read what is about to happen. :func:`compute` turns a document into a change set, :func:`diff`
renders that change set as one unified diff, and :func:`digest` fingerprints it so the thing
finally applied is provably the thing that was shown.

Nothing here writes into the project. The changes are worked out in a throwaway copy of it,
by running the same scaffolders `synqt add entity` and `synqt add contract` run, so the files
a plan promises are the files those commands would actually produce rather than a second
guess at their output.
"""

from __future__ import annotations

import difflib
import hashlib
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from . import addentity, check as checkmod, config as configmod
from . import designdoc, yamledit

# Copied into the working tree and compared afterwards: everything else is build output, a
# repository, or the editor's own layout file, and none of it is the project's source.
_IGNORED = ("build", ".git", ".synqt", "__pycache__", "node_modules", ".venv")

# The entity fields the document models. Anything else in an entity block (TLS files,
# provider settings, an env file) is the author's and is left where it is.
_ENTITY_FIELDS = ("kind", "capability", "blueprint", "provider", "targets", "identity")
_LINK_FIELDS = ("contract", "owner", "consumers", "instance", "transport")


class DesignPlanError(Exception):
    """A plan error surfaced to the CLI or the editor (no traceback)."""


@dataclass(frozen=True)
class Change:
    """One file this plan would create, rewrite, or delete, with the reason for it."""

    action: str
    path: str
    reason: str
    before: Optional[str]
    after: Optional[str]


@dataclass(frozen=True)
class Plan:
    """A whole change set: what it would do, what validation says of the result, and
    whether the project has moved under the document since it was read."""

    changes: Tuple[Change, ...]
    findings: Tuple[str, ...]
    ok: bool
    git: str
    stale: bool


# Computing


def compute(project_dir: os.PathLike[str] | str, document: Dict[str, Any], *,
            profile: Optional[str] = None) -> Plan:
    """The change set `document` implies for the project at `project_dir`."""
    root = Path(project_dir)
    current = designdoc.read(root, profile=profile)
    stale = bool(document.get("sourceHash")) and \
        document["sourceHash"] != current["sourceHash"]

    wanted, reasons = _settled(current, document)
    with tempfile.TemporaryDirectory(prefix="synqt-design-") as scratch:
        work = Path(scratch) / root.name
        _mirror(root, work)
        removed = _apply(work, current, wanted, reasons)
        changes = _changes(root, work, removed, reasons)

    base = configmod.load(root, profile=profile)
    ok, findings = checkmod.validate(designdoc.to_config(wanted, base=base),
                                     project_dir=root)
    return Plan(changes=tuple(changes), findings=tuple(findings), ok=ok,
                git=_git_position(root), stale=stale)


def _note(reasons: Dict[str, List[str]], path: str, why: str) -> None:
    """Record why `path` is in the change set. A file can be there for several reasons at
    once, and a reader deciding whether to apply the plan needs all of them."""
    causes = reasons.setdefault(path, [])
    if why not in causes:
        causes.append(why)


def _reason(reasons: Dict[str, List[str]], path: str, fallback: str) -> str:
    """The reasons for `path`, or for the directory it was scaffolded into."""
    causes = reasons.get(path) or reasons.get(path.split("/")[0] + "/")
    return "; ".join(causes or [fallback])


def _settled(current: Dict[str, Any],
             document: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, List[str]]]:
    """The document with names that no longer exist taken off every consumer list.

    Deleting an entity in the editor is one gesture, and the links that named it are not
    expected to be tidied up by hand afterwards. The tidying is recorded as a reason so it
    shows up in the plan rather than happening quietly.
    """
    reasons: Dict[str, List[str]] = {}
    alive = {entity["name"] for entity in document.get("entities", [])}
    settled = dict(document)
    settled["entities"] = [dict(entity) for entity in document.get("entities", [])]
    links = []
    for link in document.get("links", []):
        link = dict(link)
        dropped = [name for name in link.get("consumers", []) if name not in alive]
        if dropped:
            link["consumers"] = [name for name in link["consumers"] if name in alive]
            _note(reasons, "synqt.yaml",
                  f"{', '.join(dropped)} no longer exists, so '{link['name']}' no longer "
                  "lists it as a consumer")
        links.append(link)
    settled["links"] = links
    return settled, reasons


def _mirror(root: Path, work: Path) -> None:
    shutil.copytree(root, work, ignore=shutil.ignore_patterns(*_IGNORED))


def _apply(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
           reasons: Dict[str, List[str]]) -> Set[str]:
    """Make the working copy look like `wanted`. Returns the directories taken out whole."""
    removed = _apply_entities(work, current, wanted, reasons)
    _apply_links(work, current, wanted, reasons)
    return removed


def _by_name(items: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    return {item["name"]: item for item in items}


def _apply_entities(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
                    reasons: Dict[str, List[str]]) -> Set[str]:
    was = _by_name(current["entities"])
    now = _by_name(wanted["entities"])
    removed: Set[str] = set()

    for name, entity in now.items():
        if name not in was:
            _scaffold_entity(work, entity)
            _note(reasons, "synqt.yaml", f"'{name}' added")
            _note(reasons, name + "/", f"scaffolded with the '{name}' entity")
            continue
        _patch(work, "entities", name, was[name], entity, _ENTITY_FIELDS,
               _entity_field, reasons)

    for name in was:
        if name in now:
            continue
        _edit_config(work, lambda text: yamledit.remove_item(text, "entities", name))
        _note(reasons, "synqt.yaml", f"'{name}' removed")
        directory = work / name
        if directory.is_dir():
            shutil.rmtree(directory)
            removed.add(name)
            _note(reasons, name, f"the '{name}' entity was removed")
    return removed


def _scaffold_entity(work: Path, entity: Dict[str, Any]) -> None:
    """Add one entity the way `synqt add entity` would, or plainly when it has no blueprint.

    The scaffolder is run rather than imitated: an entity the editor draws has to be the
    same entity the command line produces, down to the schema file and the credential name
    written into .env.example, or the two ways into a project drift apart.
    """
    blueprint = entity.get("blueprint") or ""
    if blueprint in addentity.BLUEPRINTS:
        addentity.scaffold(work, entity["name"], blueprint, entity.get("provider") or None)
    else:
        block = {"name": entity["name"], "kind": entity.get("kind") or "service"}
        _edit_config(work, lambda text: yamledit.append_item(text, "entities", block))
    fields = {key: _entity_field(entity, key) for key in _ENTITY_FIELDS
              if _entity_field(entity, key) is not None}
    fields.pop("blueprint", None)
    fields.pop("provider", None)
    if fields:
        _edit_config(work, lambda text: yamledit.patch_item(
            text, "entities", entity["name"], fields))


def _apply_links(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
                 reasons: Dict[str, List[str]]) -> None:
    was = _by_name(current["links"])
    now = _by_name(wanted["links"])

    for name, link in now.items():
        _write_contract(work, link, was.get(name), reasons)
        if name not in was:
            block = {key: _link_field(link, key) for key in _LINK_FIELDS
                     if _link_field(link, key) is not None}
            block = {"name": name, **block}
            _edit_config(work, lambda text: yamledit.append_item(
                text, "connect_points", block))
            _note(reasons, "synqt.yaml", f"connect point '{name}' added")
            continue
        _patch(work, "connect_points", name, was[name], link, _LINK_FIELDS,
               _link_field, reasons)

    kept = {link["contract"] for link in now.values() if link.get("contract")}
    for name, link in was.items():
        if name in now:
            continue
        _edit_config(work, lambda text: yamledit.remove_item(text, "connect_points", name))
        _note(reasons, "synqt.yaml", f"connect point '{name}' removed")
        contract = link.get("contract")
        if contract and contract not in kept:
            source = work / "shared" / f"{contract}.syn"
            if source.exists():
                source.unlink()
                _note(reasons, f"shared/{contract}.syn",
                      f"no connect point carries the {contract} contract any more")


def _write_contract(work: Path, link: Dict[str, Any], was: Optional[Dict[str, Any]],
                    reasons: Dict[str, List[str]]) -> None:
    contract = link.get("contract")
    if not contract:
        return
    members = link.get("members") or []
    if was is not None and was.get("members") == members and was.get("contract") == contract:
        return
    source = work / "shared" / f"{contract}.syn"
    if not members and not source.exists():
        return
    relative = f"shared/{contract}.syn"
    # A rewrite is written whole, so a hand-written comment in the file does not survive
    # one. That is why it happens only when the members actually differ, and why the diff
    # shows the loss rather than the plan absorbing it silently.
    _note(reasons, relative, f"the {contract} contract was drawn afresh"
          if source.exists() else f"the {contract} contract is new")
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(designdoc.render_contract(contract, members), encoding="utf-8")


def _patch(work: Path, list_path: str, name: str, was: Dict[str, Any],
           now: Dict[str, Any], keys: Tuple[str, ...], field: Any,
           reasons: Dict[str, List[str]]) -> None:
    """Set what changed on one item, and unset what the document no longer carries."""
    set_fields: Dict[str, Any] = {}
    unset: List[str] = []
    for key in keys:
        before, after = field(was, key), field(now, key)
        if before == after:
            continue
        if after is None:
            unset.append(key)
        else:
            set_fields[key] = after
    if set_fields:
        _edit_config(work, lambda text: yamledit.patch_item(
            text, list_path, name, set_fields))
    for key in unset:
        _edit_config(work, lambda text: yamledit.remove_field(text, list_path, name, key))
    if set_fields or unset:
        _note(reasons, "synqt.yaml",
              f"'{name}': {', '.join(sorted(list(set_fields) + unset))} changed")


def _entity_field(entity: Dict[str, Any], key: str) -> Any:
    """One entity field as synqt.yaml spells it, or None when the file should not carry it."""
    value = entity.get(key)
    if key == "identity":
        return True if value else None
    if key == "provider":
        return {"name": value} if value else None
    if key == "targets":
        return list(value) if value else None
    if key == "kind":
        return str(value or "service")
    return str(value) if value else None


def _link_field(link: Dict[str, Any], key: str) -> Any:
    value = link.get(key)
    if key == "consumers":
        return list(value or [])
    if key == "instance":
        return str(value or "shared")
    return str(value) if value else None


def _edit_config(work: Path, edit: Any) -> None:
    path = work / "synqt.yaml"
    text = path.read_text(encoding="utf-8") if path.exists() else "entities: []\n"
    try:
        path.write_text(edit(text), encoding="utf-8")
    except yamledit.YamlEditError as error:
        raise DesignPlanError(f"synqt.yaml: {error}") from error


# The change set


def _relative_files(root: Path) -> Dict[str, Path]:
    found: Dict[str, Path] = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if any(part in _IGNORED for part in relative.parts):
            continue
        if path.is_file():
            found[relative.as_posix()] = path
    return found


def _text_of(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return f"<{path.stat().st_size} bytes, not text>\n"


def _changes(root: Path, work: Path, removed: Set[str],
             reasons: Dict[str, List[str]]) -> List[Change]:
    was = _relative_files(root)
    now = _relative_files(work)
    changes: List[Change] = []

    for relative in sorted(now):
        after = _text_of(now[relative])
        if relative not in was:
            changes.append(Change("create", relative,
                                  _reason(reasons, relative, "drawn in the editor"),
                                  None, after))
            continue
        before = _text_of(was[relative])
        if before != after:
            changes.append(Change("edit", relative,
                                  _reason(reasons, relative, "drawn in the editor"),
                                  before, after))

    for name in sorted(removed):
        inside = sorted(r for r in was if r == name or r.startswith(name + "/"))
        changes.append(Change("delete", name,
                              _reason(reasons, name, "removed in the editor"),
                              "\n".join(inside) + "\n", None))

    for relative in sorted(was):
        if relative in now or any(relative == name or relative.startswith(name + "/")
                                  for name in removed):
            continue
        changes.append(Change("delete", relative,
                              _reason(reasons, relative, "removed in the editor"),
                              _text_of(was[relative]), None))
    return changes


def diff(plan: Plan) -> str:
    """The whole change set as one unified diff, in the order it would be applied."""
    out: List[str] = []
    for change in plan.changes:
        out.append(f"# {change.action} {change.path}: {change.reason}\n")
        out.extend(difflib.unified_diff(
            (change.before or "").splitlines(keepends=True),
            (change.after or "").splitlines(keepends=True),
            fromfile=f"a/{change.path}" if change.before is not None else "/dev/null",
            tofile=f"b/{change.path}" if change.after is not None else "/dev/null",
            n=3))
        if not out[-1].endswith("\n"):
            out.append("\n")
    return "".join(out)


def digest(plan: Plan) -> str:
    """A fingerprint of the change set, so what is applied is what was shown."""
    return hashlib.sha256(diff(plan).encode("utf-8")).hexdigest()


def _git_position(root: Path) -> str:
    """Whether the project has uncommitted work, so a destructive plan can say so."""
    try:
        finished = subprocess.run(["git", "status", "--porcelain"], cwd=str(root),
                                  capture_output=True, text=True, check=False)
    except OSError:
        return "not a repository"
    if finished.returncode != 0:
        return "not a repository"
    return "dirty" if finished.stdout.strip() else "clean"
