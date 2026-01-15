# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt new``: scaffold a new project (the npm-shaped entry point).

Writes a minimal but complete topology (a client and a web edge) plus the folders,
the .gitignore that keeps mesh keys and the toolchain cache out of git, and CMake
presets. It prints the client GPLv3 conveyance reminder, because the client is served to
every visitor and its source obligation is real (docs/licensing.md).
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml

from . import addentity, appgen, appmodel, licenses, presets, toolchain

QT_VERSION = toolchain.QT_VERSION

# The qmlformat settings a scaffolded project gets, and the only source of truth for them
# (there is no copy anywhere. See QmlFormatSettingsSourceTest). Inline rather than read from
# disk because the released CLI is a PyInstaller --onefile binary with no data files, and a
# `synqt new` that set check.qml_format without shipping the settings would warn on every
# check from the very first one.
QMLFORMAT_INI = """; SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
; SPDX-License-Identifier: Apache-2.0
;
; How `synqt check` judges QML formatting when check.qml_format is on. Shipping one at all
; is the point. Left to itself qmlformat searches per directory and then falls back to a
; PER-USER file (~/.config/.qmlformat.ini), so the same QML would get a different answer on
; each machine and a third in CI. `synqt check` passes this with -s, overriding the lookup,
; and skips the check entirely if this file is missing rather than guessing.
;
; Indentation, tabs, newlines and semicolons are unambiguous and qmlformat is right
; about them. Everything that reorders or rewrites your code is off, and each is off for a
; measured reason, not out of caution:
;
;   NormalizeOrder=false            The two ordering knobs, mutually exclusive. Normalize
;   GroupAttributesTogether=false   sorts each group alphabetically (visible, width, height
;                                   -> height, title, visible, width), pulling related
;                                   properties apart. Grouping does the same reordering
;                                   without the sort, and is faithful to the conventions:
;                                   an assignment like `width: 10` is an object property
;                                   (group 5), only `property int x` is a declaration
;                                   (group 2). Both then demote an object's own state below
;                                   its logic. That reads fine on a visual Item and badly
;                                   on the two shapes SynQt has: a Source, whose
;                                   props ARE the contract and belong at the top, and a
;                                   client root, whose `visible/width/height` are what make
;                                   it a window at all. Neither moves the comment that
;                                   explains a property with it, so grouping strands them.
;                                   The conventions were written for scene objects. Where
;                                   they fit, order by hand.
;   MaxColumnWidth=-1               No wrapping. qmlformat reflows expressions and no
;                                   setting stops it, so a limit only chooses where it goes
;                                   wrong. It breaks wherever the limit lands, between an
;                                   operand and its operator or between a call and its
;                                   argument. Unset, it instead joins a hand-wrapped
;                                   expression back onto one long line, which is at least
;                                   predictable, and a line that ends up too long is a fair
;                                   signal to name a property or extract a function.
;   ObjectsSpacing=false            Blank-line insertion, which only does anything when one
;   FunctionsSpacing=false          of the reordering knobs above is on.
;   SortImports=false               Reorders imports, and qmlformat's own help warns it can
;                                   change semantics when two modules export one name.
;
; So what is left is whitespace and semicolons. The parts with one right answer, that no
; reviewer should spend a comment on. Change any of it. It is your project's QML.

UseTabs=false
IndentWidth=4
NewlineType=unix
SemicolonRule=always
NormalizeOrder=false
GroupAttributesTogether=false
MaxColumnWidth=-1
ObjectsSpacing=false
FunctionsSpacing=false
SortImports=false
"""

_MAIN_QML = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root

    visible: true
    width: 360
    height: 240
    title: "SynQt app"

    // Surfaces the connection state to the browser console. A boot sentinel `synqt dev`
    // (and the browser end-to-end check) watch for. Invisible. Harmless in the shipped app.
    Item {
        property string status: "state=" + Session.state
        onStatusChanged: console.log("SynQt client: " + status)
        Component.onCompleted: console.log("SynQt client booted")
    }

    Label {
        anchors.centerIn: parent
        // On one line, because the scaffold ships with check.qml_format on, and
        // qmlformat reflows a wrapped expression, so a hand-wrapped ternary here would
        // report the new project as unformatted on its very first `synqt check`.
        text: Session.state === "connected" ? "Connected" : "Connecting..."
    }
}
"""


def write_client_main(project_dir: os.PathLike[str] | str,
                      entity: Dict[str, Any]) -> Optional[str]:
    """Give a client entity the one file it cannot start without, unless it has one.

    The generated client main.cpp does `engine.loadFromModule(uri, "Main")`, so `Main.qml` is
    the root object and its name is not a choice. A client with no such file builds, loads,
    logs nothing and renders a blank page, which is why it is written with the entity rather
    than left to be remembered. `synqt new` writes it, and so does a client drawn in the
    editor. Returns the project-relative path when it wrote one.
    """
    relative = appmodel.entity_file_path(entity)
    target = Path(project_dir) / relative
    if target.exists():
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(_MAIN_QML, encoding="utf-8")
    return relative


def entity_singleton(name: str) -> str:
    """An entity's own QML, while the entity exports nothing: one object, alive as long as
    the entity is.

    A singleton because there is one of this entity. ``appmodel.discover_singletons`` finds
    it by its ``pragma Singleton`` and the generated main registers it under the entity's own
    QML module, so anything the entity owns reaches it by name. Exporting a connect point
    turns this same file into that point's Source (:func:`synqt.addcontract.write_source`),
    because an entity and the surface it exports are one file.

    Written the way ``qmlformat`` would write it, so a scaffolded project passes its own
    ``synqt check`` with nothing to reformat first.
    """
    type_name = f"{name[:1].upper()}{name[1:]}"
    return ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
            "// SPDX-License-Identifier: Apache-2.0\n"
            "\n"
            "pragma Singleton\n"
            "\n"
            "import QtQuick\n"
            "\n"
            f"// The '{name}' entity itself: one of it, for as long as the entity runs, and one\n"
            "// whatever the entity answers to `shared:`. State that belongs to the whole\n"
            "// entity goes here rather than in a Source when the entity is not shared,\n"
            "// because a Source is then one caller's and dies with them.\n"
            f"// Every Source this entity owns reaches it as "
            f"`{type_name}`.\n"
            "QtObject {\n"
            "    id: root\n"
            "}\n")


def write_entity_qml(project_dir: os.PathLike[str] | str,
                     entity: Dict[str, Any]) -> Optional[str]:
    """Give an entity its own file, unless it has one. Returns the path when it wrote one.

    Every entity has one from the moment it exists, before it owns or consumes anything. An
    entity that is in synqt.yaml with an empty directory beside it is an entity nobody can
    open.
    """
    if appmodel.is_client(entity):
        return write_client_main(project_dir, entity)
    relative = appmodel.entity_file_path(entity)
    target = Path(project_dir) / relative
    if target.exists():
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(entity_singleton(str(entity.get("name") or "")), encoding="utf-8")
    return relative


class NewProjectError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _config(name: str, entities: List[Dict[str, Any]]) -> Dict[str, Any]:
    return {
        # No `origin_model`. Absent means same-origin, which is what a scaffold should be: the
        # session cookie is first-party, so nothing here depends on a browser policy that is
        # being withdrawn. `split_origin` is written by hand, by someone who has read what it
        # costs; see docs/project-layout-and-config.md.
        "project": {"name": name, "version": "0.1.0", "qt_version": QT_VERSION},
        "scopes": {"order": ["anonymous", "user", "moderator", "admin"],
                   "hierarchical": True, "default": "anonymous"},
        "security": {"allowed_origins": ["self"], "cross_origin_isolation": False},
        # Single-threaded WASM runs in every browser; set client_threads: multi to build the
        # threaded client (implies cross-origin isolation; needs COOP/COEP, emitted for you).
        "build": {"client_threads": "single"},
        # On from the first commit, while the QML is still format-clean: adopting a
        # formatter later means one enormous reformatting diff nobody reviews. It reports,
        # never rewrites, and never fails the check. The rules are the project's own
        # .qmlformat.ini; edit it or set this to false.
        "check": {"qml_format": True},
        "entities": entities,
        "connect_points": [],
    }


def _write_qmlformat_settings(root: Path) -> None:
    """Write the project's qmlformat settings, so check.qml_format has rules to judge by."""
    (root / ".qmlformat.ini").write_text(QMLFORMAT_INI)


def scaffold(parent_dir: os.PathLike[str] | str, name: str, *,
             auth: Optional[str] = None,
             starting: Optional[List[Tuple[str, str]]] = None) -> str:
    """Write a new project: a client, a web edge, and whatever `starting` names.

    `starting` is `(name, type)` pairs and comes only from `synqt create`, which asks for
    both. `synqt new` has no flag for it. An entity is something somebody named, naming one
    on a project-creation flag meant a `<name>:<type>` pair nobody enjoyed writing, and
    `synqt add entity <name> --type <type>` is the one shape that already says it.
    """
    root = Path(parent_dir) / name
    if root.exists() and any(root.iterdir()):
        raise NewProjectError(f"{root} already exists and is not empty")
    root.mkdir(parents=True, exist_ok=True)

    # Named for what they are rather than for their type, because the type is already the
    # folder they sit in: the client is `client/app/`, the edge is `web/edge/`. An entity
    # called `web` would land in `web/web/`, and every entity of that type after it would
    # have to explain why it was not allowed the same name.
    entities: List[Dict[str, Any]] = [
        {"name": "app", "type": "client", "targets": ["wasm"]},
        # The edge ships with TLS to the browser already configured, pointing at the
        # conventional place for the certificate. `synqt dev` runs plaintext on localhost
        # and ignores it. `synqt build --release` and `synqt serve` require either this or
        # public.tls_terminated_upstream, so a new project meets that rule from its first
        # release build rather than discovering it at the deployment.
        {"name": "edge", "type": "web_edge",
         "tls": {"cert_file": "certs/edge/fullchain.pem",
                 "key_file": "certs/edge/privkey.pem"}},
    ]
    config = _config(name, entities)
    if auth:
        # Mark the edge so the license generator knows it links Network Authorization.
        config["entities"][1]["identity"] = True
    (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

    for entity in entities:
        write_entity_qml(root, entity)

    (root / ".gitignore").write_text(
        "# SynQt: never commit mesh private keys, the toolchain cache, or anything\n"
        "# generated. generated/ holds the build and one main.cpp per entity, written\n"
        "# from synqt.yaml every time. CMakeLists.txt at the root is yours and is kept;\n"
        "# the presets beside it are regenerated with the toolchain they point at.\n"
        f"{appmodel.GENERATED_DIR}/\nbuild/\n/CMakePresets.json\n/CMakeUserPresets.json\n"
        "synqt/toolchain/\nsynqt/mesh/*.key\n"
        "synqt/mesh/dev/\n.env\n")
    (root / ".env.example").write_text("# Entity secrets (env: references), never committed\n")
    _write_qmlformat_settings(root)

    # A starting entity is scaffolded by `synqt add entity` itself, so the two paths cannot
    # drift, with the same config block, same provider defaults, same folder and same entity file.
    # Writing a bare `{name, kind, blueprint}` here would leave a project whose
    # relational entity has no provider settings and no schema, unlike the same entity added
    # a command later. It runs after .env.example exists because an external provider
    # appends its secret to it.
    for entity_name, entity_type in starting or []:
        addentity.scaffold(root, entity_name, entity_type)
    config = yaml.safe_load((root / "synqt.yaml").read_text())

    presets.write(root, config)
    # The buildable app. The multi-binary CMakeLists and one main.cpp per entity, derived
    # from the topology so the scaffold compiles as-is (client + edge, no connect points).
    appgen.generate(root, config)

    lines = [
        f"Scaffolded '{name}'. Next:",
        f"  cd {name} && synqt dev",
        "",
        licenses.CLIENT_GPL_WARNING,
    ]
    if auth:
        lines.insert(1, f"  Auth requested ({auth}): finish it with 'synqt add auth {auth}'.")
    return "\n".join(lines)
