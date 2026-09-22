# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt new``: scaffold a new project.

Writes a client and a web edge, the folders, a .gitignore that keeps mesh keys and the
toolchain cache out of git, and the CMake presets. Prints the client GPLv3 conveyance
reminder (docs/licensing.md).
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml

from . import addentity, appgen, appmodel, devidentities, licenses, presets, toolchain

QT_VERSION = toolchain.QT_VERSION

# The qmlformat settings of a scaffolded project, and their only copy (see
# QmlFormatSettingsSourceTest). Inline, because the frozen CLI has no data files.
QMLFORMAT_INI = """; SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
; SPDX-License-Identifier: Apache-2.0
;
; How `synqt check` judges QML formatting when check.qml_format is on. Without this file
; qmlformat searches per directory and then falls back to a per-user file
; (~/.config/.qmlformat.ini), so the same QML would get a different answer on each machine
; and a third in CI. `synqt check` passes this file with -s, which overrides that lookup,
; and skips the check when the file is missing.
;
; Indentation, tabs, newlines and semicolons are unambiguous and qmlformat is right
; about them. Everything that reorders or rewrites your code is off, each for the reason
; below:
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
; What is left is whitespace and semicolons. Change any of it: it is your project's QML.

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

import SynQt
import QtQuick.Controls

ApplicationWindow {
    id: root

    visible: true
    width: 360
    height: 240
    title: "SynQt app"

    // Logs the connection state to the browser console. `synqt dev` and the browser
    // end-to-end check watch for these lines. It draws nothing.
    Item {
        property string status: "state=" + Session.state
        onStatusChanged: console.log("SynQt client: " + status)
        Component.onCompleted: console.log("SynQt client booted")
    }

    Label {
        anchors.centerIn: parent
        // On one line: qmlformat reflows a wrapped expression, and the scaffold ships
        // with check.qml_format on.
        text: Session.state === "connected" ? "Connected" : "Connecting..."
    }
}
"""


def write_client_main(project_dir: os.PathLike[str] | str,
                      entity: Dict[str, Any]) -> Optional[str]:
    """Give a client entity its `Main.qml`, unless it has one. The generated main.cpp loads
    "Main" as the root object. Returns the project-relative path when it wrote one.
    """
    relative = appmodel.entity_file_path(entity)
    target = Path(project_dir) / relative
    if target.exists():
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(_MAIN_QML, encoding="utf-8")
    return relative


def entity_singleton(name: str) -> str:
    """An entity's own QML while the entity exports nothing: one object, alive as long as the
    entity.

    Marked ``pragma Shared``, so ``appmodel.discover_singletons`` finds it and the generated
    main registers it; ``synqt build`` writes ``pragma Singleton`` into the copy under
    ``generated/`` (:mod:`synqt.qmlrewrite`). Exporting a connect point turns this file into
    the Source (:func:`synqt.addcontract.write_source`). Formatted as ``qmlformat`` would.
    """
    type_name = f"{name[:1].upper()}{name[1:]}"
    return ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
            "// SPDX-License-Identifier: Apache-2.0\n"
            "\n"
            f"pragma {appmodel.SHARED_PRAGMA}\n"
            "\n"
            "import SynQt\n"
            "\n"
            f"// The '{name}' entity itself: one instance for as long as the entity runs,\n"
            "// whatever `shared:` says. State that belongs to the whole entity goes here,\n"
            "// because an unshared entity's Source belongs to one caller and ends with them.\n"
            f"// Every Source this entity owns reaches it as "
            f"`{type_name}`.\n"
            "QtObject {\n"
            "    id: root\n"
            "}\n")


def write_entity_qml(project_dir: os.PathLike[str] | str,
                     entity: Dict[str, Any]) -> Optional[str]:
    """Give an entity its own file, unless it has one. Returns the path when it wrote one."""
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
        # No `origin_model`: absent means same-origin, with a first-party session cookie.
        # `split_origin` is a hand edit (docs/project-layout-and-config.md).
        "project": {"name": name, "version": "0.1.0", "qt_version": QT_VERSION},
        "scopes": {"order": ["anonymous", "user", "moderator", "admin"],
                   "hierarchical": True, "default": "anonymous"},
        "security": {"allowed_origins": ["self"], "cross_origin_isolation": False},
        # Single-threaded WASM runs in every browser. `client_threads: multi` builds the
        # threaded client (implies cross-origin isolation; COOP/COEP are emitted).
        "build": {"client_threads": "single"},
        # On from the start, while the QML is format-clean. It reports, never rewrites, and
        # never fails the check. The rules are in .qmlformat.ini.
        "check": {"qml_format": True},
        # Written empty so the keys are visible. Empty values are the same as no block, and
        # `synqt check` lists the blank ones. `cookies` stays empty: the session credential
        # is exempt under Article 5(3) of the ePrivacy Directive, and adding a category
        # makes CookieConsent appear (docs/privacy.md).
        "privacy": {"policy": "", "legal_notice": "", "contact": "", "cookies": []},
        "entities": entities,
        "connect_points": [],
    }


def _write_qmlformat_settings(root: Path) -> None:
    """Write the project's qmlformat settings, so check.qml_format has rules to judge by."""
    (root / ".qmlformat.ini").write_text(QMLFORMAT_INI, encoding="utf-8")


def write_gitignore(root: Path) -> None:
    """The .gitignore every SynQt project gets, scaffolded or copied from an example. Keeps
    mesh private keys and the toolchain cache out of git.
    """
    (root / ".gitignore").write_text(
        "# SynQt: never commit mesh private keys, the toolchain cache, or generated files.\n"
        "# generated/ holds the build and one main.cpp per entity, rewritten from\n"
        "# synqt.yaml. The root CMakeLists.txt is yours; the presets beside it are\n"
        "# regenerated.\n"
        f"{appmodel.GENERATED_DIR}/\nbuild/\n/CMakePresets.json\n/CMakeUserPresets.json\n"
        "synqt/toolchain/\nsynqt/mesh/*.key\n"
        # The container CA certificate `synqt docker ca` writes. Public, but
        # machine-specific.
        "synqt/mesh/dev/\nsynqt/mesh/docker-ca.crt\n.env\n"
        # The people this developer signs in as under `synqt dev --identity-picker`.
        f"{devidentities.FILE_NAME}\n"
        # Where a relational entity on the embedded engine keeps its database.
        "db/relational/*/data/\n", encoding="utf-8")


def scaffold(parent_dir: os.PathLike[str] | str, name: str, *,
             auth: Optional[str] = None,
             starting: Optional[List[Tuple[str, str]]] = None) -> str:
    """Write a new project: a client, a web edge, and whatever `starting` names.

    `starting` is `(name, type)` pairs from `synqt create`. `synqt new` has no such flag;
    use `synqt add entity <name> --type <type>`.
    """
    root = Path(parent_dir) / name
    # `name` may be a path (`../shop`, `.`); the project is named after its last component.
    project_name = root.resolve().name
    if not appmodel.is_valid_project_name(project_name):
        raise NewProjectError(
            f"'{project_name}' cannot name a project: a project name starts with a letter and "
            "is made of letters, digits, underscores and hyphens, because it becomes the "
            "CMake project, the client's QML module and the container names")
    if root.exists() and any(root.iterdir()):
        raise NewProjectError(f"{root} already exists and is not empty")
    root.mkdir(parents=True, exist_ok=True)

    # Named for their role: the client is `client/app/`, the edge `web/edge/`.
    entities: List[Dict[str, Any]] = [
        {"name": "app", "type": "client", "targets": ["wasm"]},
        # The edge is scaffolded with browser TLS pointing at the conventional certificate
        # path. `synqt dev` runs plaintext and ignores it; `synqt build --release` and
        # `synqt serve` require this or public.tls_terminated_upstream.
        {"name": "edge", "type": "web_edge",
         "tls": {"cert_file": "certs/edge/fullchain.pem",
                 "key_file": "certs/edge/privkey.pem"}},
    ]
    config = _config(project_name, entities)
    if auth:
        # Mark the edge so the license generator knows it links Network Authorization.
        config["entities"][1]["identity"] = True
    (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")

    for entity in entities:
        write_entity_qml(root, entity)

    write_gitignore(root)
    (root / ".env.example").write_text(
        "# Entity secrets (env: references), never committed\n", encoding="utf-8")
    _write_qmlformat_settings(root)

    # A starting entity is scaffolded by `synqt add entity` itself, after .env.example
    # exists (an external provider appends its secret there).
    for entity_name, entity_type in starting or []:
        addentity.scaffold(root, entity_name, entity_type)
    config = yaml.safe_load((root / "synqt.yaml").read_text(encoding="utf-8"))

    presets.write(root, config)
    # The buildable app: the CMake and one main.cpp per entity, from the topology.
    appgen.generate(root, config)

    lines = [
        f"Scaffolded '{project_name}'. Next:",
        f"  cd {name} && synqt dev",
        "",
        licenses.CLIENT_GPL_WARNING,
    ]
    if auth:
        lines.insert(1, f"  Auth requested ({auth}): finish it with 'synqt add auth {auth}'.")
    return "\n".join(lines)
