# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What applying a design document would do to a project, worked out before any of it runs.

The editor may make any change, deletions included, and nothing runs until the plan has been
shown. :func:`compute` turns a document into a change set, :func:`diff` renders it as one
unified diff, and :func:`digest` fingerprints it so what is applied is what was shown. The
changes are computed in a throwaway copy of the project, with the same scaffolders `synqt
add entity` and `synqt add connect-point` use.
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

from . import (addcontract, addentity, appmodel, check as checkmod, config as configmod,
               monitorscaffold)
from . import designdoc, newproject, qmlcomments, yamledit

# Copied into the working tree and compared afterwards. Everything else is build output, a
# repository, or the editor layout file. `generated/` is excluded: the scaffolders
# regenerate it and the next build rewrites it, so it is not a change to review.
_IGNORED = ("build", "generated", ".git", ".synqt", "__pycache__", "node_modules", ".venv")

# The entity fields the document models. Everything else in an entity block stays.
_ENTITY_FIELDS = ("type", "provider", "targets", "identity", "shared", "bundles")
_LINK_FIELDS = ("owner", "consumers", "transport", "scope", "behind", "export")


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
    """A whole change set: what it would do, what validation says of the result, and whether
    the project changed since the document was read.
    """

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
    base = configmod.load(root, profile=profile)
    stale = bool(document.get("sourceHash")) and \
        document["sourceHash"] != current["sourceHash"]

    wanted, reasons = _settled(current, document)
    with tempfile.TemporaryDirectory(prefix="synqt-design-") as scratch:
        work = Path(scratch) / root.name
        _mirror(root, work)
        removed = _apply(work, current, wanted, reasons, base)
        changes = _changes(root, work, removed, reasons)

    ok, findings = checkmod.validate(
        _with_scaffolded_monitors(designdoc.to_config(wanted, base=base)), project_dir=root)
    unwritable = _uncompilable_contracts(wanted)
    return Plan(changes=tuple(changes), findings=tuple(findings) + tuple(unwritable),
                ok=ok and not unwritable, git=_git_position(root), stale=stale)


def _with_scaffolded_monitors(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` as it will be once the monitor scaffolder has run over the drawn monitors.

    A drawn monitor becomes four things: the entity, the console client, its sign-in gate,
    and the `monitoring.entity` line. The plan validates the configuration, so the
    scaffolder's own three functions are applied first. A monitor that is already wired
    keeps what it has.
    """
    entities = appmodel.entities(config)
    drawn = [entity for entity in entities
             if appmodel.entity_type(entity) == "monitor"
             and not isinstance(entity.get("bundles"), dict)]
    if not drawn:
        return config
    settled = dict(config)
    settled["entities"] = [dict(entity) for entity in entities]
    by_name = {str(entity.get("name") or ""): entity for entity in settled["entities"]}
    for entity in drawn:
        name = str(entity.get("name") or "")
        console = f"{name}-console"
        block = monitorscaffold.monitor_block(name, config)
        block["bundles"] = monitorscaffold.bundles_block(console)
        by_name[name].update({key: value for key, value in block.items()
                              if key not in by_name[name]})
        if console not in by_name:
            settled["entities"].append(monitorscaffold.console_block(console, name))
    if not appmodel.monitor_entity(settled):
        settled["monitoring"] = {**(settled.get("monitoring") or {}),
                                 "entity": str(drawn[0].get("name") or "")}
    return settled


def _uncompilable_contracts(wanted: Dict[str, Any]) -> List[str]:
    """Every drawn contract the compiler would refuse to read back.

    A member name typed in the panel may not be a valid name (`record` opens a record
    declaration). Written out, it would leave a project the editor cannot open, so each
    contract is rendered and parsed here.
    """
    problems: List[str] = []
    for link in wanted.get("links", []):
        contract = appmodel.contract_of(link)
        if not contract or not (link.get("members") or []):
            continue
        try:
            designdoc.parse_export(
                contract, {"owner": link.get("owner"),
                           "export": designdoc.render_export(link["members"])})
        except designdoc.DesignDocError as error:
            problems.append(f"error: '{link.get('name')}': the {contract} contract would "
                            f"not compile: {error}")
    return problems


def _note(reasons: Dict[str, List[str]], path: str, why: str) -> None:
    """Record why `path` is in the change set. A file can be there for several reasons."""
    causes = reasons.setdefault(path, [])
    if why not in causes:
        causes.append(why)


def _reason(reasons: Dict[str, List[str]], path: str, fallback: str) -> str:
    """The reasons for `path`, or for the directory it was scaffolded into."""
    causes = reasons.get(path) or reasons.get(path.split("/")[0] + "/")
    return "; ".join(causes or [fallback])


def _settled(current: Dict[str, Any],
             document: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, List[str]]]:
    """The document with deleted names taken off every consumer list, each removal recorded as
    a reason.
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
           reasons: Dict[str, List[str]], base: Dict[str, Any]) -> Set[str]:
    """Make the working copy look like `wanted`. Returns the directories taken out whole."""
    _apply_project(work, current, wanted, reasons)
    _apply_scopes(work, current, wanted, reasons)
    removed = _apply_entities(work, current, wanted, reasons)
    _apply_links(work, current, wanted, reasons, base)
    return removed


def _apply_project(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
                   reasons: Dict[str, List[str]]) -> None:
    """Carry a renamed project into `project.name`. An empty name is ignored."""
    was = str(current.get("project") or "")
    now = str(wanted.get("project") or "").strip()
    if not now or now == was:
        return
    _edit_config(work, lambda text: yamledit.set_scalar(text, "project.name", now))
    _note(reasons, "synqt.yaml", f"the project is called '{now}' now")


def _apply_scopes(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
                  reasons: Dict[str, List[str]]) -> None:
    """Carry an edited scope vocabulary into `scopes:`.

    The order is the authority ranking and the generated Scope enum values, so a reorder
    renumbers every hook; it gets its own line in the change set. A rename changes the list
    only: gates, bundle keys and mapping hooks are left as they are, and `synqt check`
    reports each one that no longer resolves. `default:` is rewritten when it names a
    removed scope.
    """
    was = [str(scope) for scope in current.get("scopes") or [] if str(scope)]
    now = [str(scope) for scope in wanted.get("scopes") or [] if str(scope)]
    if not now or now == was:
        return
    declared = configmod.load(work).get("scopes")
    if not isinstance(declared, dict):
        # No section yet: write it whole, with the settings that belong beside the order.
        _edit_config(work, lambda text: yamledit.set_scalar(text, "scopes", {
            "order": now, "hierarchical": True, "default": now[0]}))
        _note(reasons, "synqt.yaml", "the project declares its scopes now: " + ", ".join(now))
        return

    _edit_config(work, lambda text: yamledit.set_scalar(text, "scopes.order", now))
    _note(reasons, "synqt.yaml", "the scopes are " + ", ".join(now) + " now")

    before = str(current.get("scopeDefault") or (was[0] if was else ""))
    after = str(wanted.get("scopeDefault") or "")
    if after and after in now:
        settled = after
    elif before in now:
        settled = before
    else:
        settled = now[0]
    if settled != before:
        _edit_config(work, lambda text: yamledit.set_scalar(text, "scopes.default", settled))
        _note(reasons, "synqt.yaml",
              f"a caller with no session holds '{settled}' now")


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
            _note(reasons, appmodel.entity_dir(entity) + "/",
                  f"scaffolded with the '{name}' entity")
            continue
        _patch(work, "entities", name, was[name], entity, _ENTITY_FIELDS,
               _entity_field, reasons)
        _write_entity_qml(work, entity, reasons)
        _write_entity_schema(work, entity, reasons)
        _write_entity_companions(work, entity, reasons)

    for name in was:
        if name in now:
            continue
        _edit_config(work, lambda text: yamledit.remove_item(text, "entities", name))
        _note(reasons, "synqt.yaml", f"'{name}' removed")
        folder = appmodel.entity_dir(was[name])
        directory = work / folder
        if directory.is_dir():
            shutil.rmtree(directory)
            removed.add(folder)
            _note(reasons, folder, f"the '{name}' entity was removed")
    return removed


def _scaffold_entity(work: Path, entity: Dict[str, Any]) -> None:
    """Add one entity the way `synqt add entity` would, whatever type it is, by running that
    scaffolder.
    """
    entity_type = appmodel.entity_type(entity)
    if entity_type in addentity.TYPES:
        try:
            addentity.scaffold(work, entity["name"], entity_type,
                               entity.get("provider") or None)
        except addentity.AddEntityError as error:
            raise DesignPlanError(f"'{entity['name']}': {error}") from error
        _uncomment(work, appmodel.entity_dir(entity))
    else:
        # A client or a web edge: the block and the entity file written below. The client
        # cannot start without its file.
        block = {"name": entity["name"], "type": entity_type}
        _edit_config(work, lambda text: yamledit.append_item(text, "entities", block))
    # Every entity gets its own file, as `synqt add entity` writes.
    written = newproject.write_entity_qml(work, entity)
    if written:
        _uncomment_file(work / written)
    fields = {key: _entity_field(entity, key) for key in _ENTITY_FIELDS
              if _entity_field(entity, key) is not None}
    fields.pop("type", None)
    fields.pop("provider", None)
    if fields:
        _edit_config(work, lambda text: yamledit.patch_item(
            text, "entities", entity["name"], fields))


def _apply_links(work: Path, current: Dict[str, Any], wanted: Dict[str, Any],
                 reasons: Dict[str, List[str]], base: Dict[str, Any]) -> None:
    was = _by_name(current["links"])
    now = _by_name(wanted["links"])
    points = {appmodel.point_name(point): point for point in appmodel.connect_points(base)}
    alive = {entity["name"] for entity in wanted["entities"]}
    # Resolve the owners once, from the drawing (new owners) and the configuration.
    owners = {str(entity.get("name") or ""): entity
              for entity in list(appmodel.entities(base)) + list(current["entities"])
              + list(wanted["entities"])}

    for name, link in now.items():
        _write_source(work, link, points, alive, owners, reasons)
        if name not in was:
            # No `name:`: the owner names the point, and `owner` is first in _LINK_FIELDS.
            block = {key: _link_field(link, key) for key in _LINK_FIELDS
                     if _link_field(link, key) is not None}
            _edit_config(work, lambda text: yamledit.append_item(
                text, "connect_points", block))
            _note(reasons, "synqt.yaml", f"connect point '{name}' added")
            continue
        _patch(work, "connect_points", name, was[name], link, _LINK_FIELDS,
               _link_field, reasons)

    for name in was:
        if name in now:
            continue
        _edit_config(work, lambda text: yamledit.remove_item(text, "connect_points", name))
        _note(reasons, "synqt.yaml", f"connect point '{name}' removed")

    # A removed link needs no cleanup: its contract was written on it.


def _write_source(work: Path, link: Dict[str, Any], points: Dict[str, Dict[str, Any]],
                  alive: Set[str], owners: Dict[str, Dict[str, Any]],
                  reasons: Dict[str, List[str]]) -> None:
    """Give a link an owner-side Source file: the edited one, or an empty one.

    A file nobody typed into is only created, never rewritten. Only text marked
    ``qmlEdited`` overwrites a file, because the document also carries copies read at page
    load that may be older than the disk.
    """
    contract, owner = appmodel.contract_of(link), link.get("owner")
    owning = owners.get(str(owner or ""))
    if not contract or owner not in alive or owning is None:
        return
    point = points.get(link["name"]) or {}
    relative = str(link.get("server") or point.get("server")
                   or appmodel.source_path(owning, contract))
    target = work / relative
    edited = _edited_qml(link)
    existing = _text_of(target) if target.exists() else None
    # Overwrite the entity file only if it is still exactly what the scaffolder wrote.
    if existing is not None and not addcontract.untouched_scaffold(existing, owning):
        if edited is not None and edited != existing:
            _note(reasons, relative, f"the Source for '{link['name']}' was edited")
            target.write_text(edited, encoding="utf-8")
        return
    members = link.get("members") or []
    if edited is not None:
        _note(reasons, relative, f"the Source for '{link['name']}' was written here")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(edited, encoding="utf-8")
        return
    _note(reasons, relative,
          f"'{link['name']}' had no Source on {owner}, so this one declares what the "
          "contract says and implements none of it" if members
          else f"'{link['name']}' had no Source on {owner}, so this one is empty")
    addcontract.write_source(work, owning, contract, point=link["name"], path=relative,
                             members=members)
    _uncomment_file(target)


def _edited_qml(item: Dict[str, Any]) -> Optional[str]:
    """The QML typed into this item in the editor, or None. Only text marked as typed is written."""
    text = item.get("qml")
    return text if item.get("qmlEdited") and isinstance(text, str) and text else None


def _write_entity_qml(work: Path, entity: Dict[str, Any],
                      reasons: Dict[str, List[str]]) -> None:
    """Keep an entity file: write what was typed into it, or create it if missing (see
    :func:`_edited_qml`).
    """
    relative = appmodel.entity_file_path(entity)
    target = work / relative
    edited = _edited_qml(entity)
    if edited is None:
        # Missing: write it fresh.
        written = newproject.write_entity_qml(work, entity)
        if written:
            _uncomment_file(work / written)
            _note(reasons, relative, f"'{entity['name']}' had no file of its own")
        return
    if target.exists() and edited == _text_of(target):
        return
    _note(reasons, relative, f"the QML for '{entity['name']}' was edited")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(edited, encoding="utf-8")


def _write_entity_schema(work: Path, entity: Dict[str, Any],
                         reasons: Dict[str, List[str]]) -> None:
    """The relational schema, when somebody typed into it. Same rule as the QML. Never created
    here: `synqt add entity` writes it with the entity.
    """
    if appmodel.entity_type(entity) != "relational" or not entity.get("schemaEdited"):
        return
    text = entity.get("schema")
    if not isinstance(text, str) or not text:
        return
    relative = f"{appmodel.entity_dir(entity)}/schema.sql"
    target = work / relative
    if target.exists() and text == _text_of(target):
        return
    _note(reasons, relative, f"the schema for '{entity['name']}' was edited")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")


def _write_entity_companions(work: Path, entity: Dict[str, Any],
                             reasons: Dict[str, List[str]]) -> None:
    """The other files in an entity folder, where somebody typed into one. Same rule as the
    QML. A path outside the entity folder, or of a kind the document never carries, is
    refused.
    """
    folder = (work / appmodel.entity_dir(entity)).resolve()
    for companion in entity.get("files") or []:
        if not isinstance(companion, dict) or not companion.get("edited"):
            continue
        path = companion.get("path")
        text = companion.get("text")
        if (not isinstance(path, str) or not isinstance(text, str)
                or Path(path).suffix not in designdoc.COMPANION_SUFFIXES):
            continue
        target = (folder / path).resolve()
        if folder not in target.parents:
            continue
        if target.exists() and text == _text_of(target):
            continue
        relative = f"{appmodel.entity_dir(entity)}/{path}"
        _note(reasons, relative, f"'{path}' in '{entity['name']}' was edited")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")


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
    if key == "type":
        return appmodel.entity_type(entity)
    if key == "shared":
        # `shared: false` is the interesting value, so it is not a truthiness test. Written
        # only when it differs from what the entity resolves to, as designdoc does.
        default = appmodel.is_shared({"type": appmodel.entity_type(entity)})
        return value if isinstance(value, bool) and value is not default else None
    return str(value) if value else None


def _link_field(link: Dict[str, Any], key: str) -> Any:
    value = link.get(key)
    if key == "consumers":
        return list(value or [])
    # Which entity serves each scope on a front. Empty means no front, so the block is
    # removed.
    if key == "behind":
        wired = {str(scope): str(name)
                 for scope, name in (value or {}).items() if scope and name}
        return wired or None
    # The contract, rendered from the document members into lines.
    if key == "export":
        return designdoc.render_export(link.get("members") or []) or None
    return str(value) if value else None


def _uncomment_file(target: Path) -> None:
    """Take the commentary out of one file this plan scaffolded. Only called on scaffolder
    output.
    """
    if not target.is_file():
        return
    text = target.read_text(encoding="utf-8")
    trimmed = qmlcomments.without_commentary(text)
    if trimmed != text:
        target.write_text(trimmed, encoding="utf-8")


def _uncomment(work: Path, folder: str) -> None:
    """The same, for every QML file a blueprint scaffolder wrote into an entity folder. The
    command line's copies keep their comments.
    """
    directory = work / folder
    if not directory.is_dir():
        return
    for path in sorted(directory.rglob("*.qml")):
        _uncomment_file(path)


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


# Applying


def execute(project_dir: os.PathLike[str] | str, plan: Plan) -> str:
    """Apply `plan` to the project, or leave it untouched.

    Everything the plan touches is held in memory, and any failure restores it before the
    error is raised. A plan that does not validate, or was computed against a changed
    synqt.yaml, is refused.
    """
    root = Path(project_dir)
    if plan.stale:
        raise DesignPlanError(
            "the project changed after this plan was worked out; read the design again "
            "and have another look at what it would do")
    if not plan.ok:
        errors = [message for message in plan.findings if message.startswith("error:")]
        raise DesignPlanError("this design does not pass synqt check: "
                              + "; ".join(errors or ["it was not accepted"]))

    held: List[Tuple[Path, Optional[bytes]]] = []
    made: List[Path] = []
    done: List[str] = []
    at = ""
    try:
        for change in plan.changes:
            at = change.path
            target = root / change.path
            _hold(target, held)
            if change.action == "delete":
                _remove(target)
            else:
                made.extend(_missing_parents(root, target))
                _write(target, change.after or "")
            done.append(f"{change.action} {change.path}: {change.reason}")
    except Exception as error:
        _restore(held, made)
        raise DesignPlanError(
            f"{at}: {error}. Nothing was changed; the project is as it was.") from error
    return "\n".join(done)


def _hold(target: Path, held: List[Tuple[Path, Optional[bytes]]]) -> None:
    """Remember what `target` is now, so it can be put back. A directory holds its tree."""
    if target.is_dir():
        for path in sorted(target.rglob("*")):
            if path.is_file():
                held.append((path, path.read_bytes()))
        return
    held.append((target, target.read_bytes() if target.is_file() else None))


def _missing_parents(root: Path, target: Path) -> List[Path]:
    """The directories writing `target` would create, nearest last."""
    missing: List[Path] = []
    parent = target.parent
    while parent != root and not parent.exists() and root in parent.parents:
        missing.append(parent)
        parent = parent.parent
    return list(reversed(missing))


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _remove(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def _restore(held: List[Tuple[Path, Optional[bytes]]], made: List[Path]) -> None:
    for path, data in reversed(held):
        if data is None:
            if path.is_file():
                path.unlink()
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    for directory in reversed(made):
        if directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()


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
