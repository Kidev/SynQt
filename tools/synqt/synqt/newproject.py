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
from typing import Any, Dict, List, Optional

import yaml

from . import appgen, licenses, presets, toolchain

QT_VERSION = toolchain.QT_VERSION

# The qmlformat settings a scaffolded project gets, and the only source of truth for them
# (there is no copy anywhere; see QmlFormatSettingsSourceTest). Inline rather than read from
# disk because the released CLI is a PyInstaller --onefile binary with no data files, and a
# `synqt new` that set check.qml_format without shipping the settings would warn on every
# check from the very first one.
QMLFORMAT_INI = """; SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
; SPDX-License-Identifier: Apache-2.0
;
; How `synqt check` judges QML formatting when check.qml_format is on. Shipping one at all
; is the point: left to itself qmlformat searches per directory and then falls back to a
; PER-USER file (~/.config/.qmlformat.ini), so the same QML would get a different answer on
; each machine and a third in CI. `synqt check` passes this with -s, overriding the lookup,
; and skips the check entirely if this file is missing rather than guessing.
;
; Indentation, tabs, newlines and semicolons are unambiguous and qmlformat is simply right
; about them. Everything that reorders or rewrites your code is off, and each is off for a
; measured reason, not out of caution:
;
;   NormalizeOrder=false            The two ordering knobs, mutually exclusive. Normalize
;   GroupAttributesTogether=false   sorts each group alphabetically (visible, width, height
;                                   -> height, title, visible, width), pulling related
;                                   properties apart; grouping does the same reordering
;                                   without the sort, and is faithful to the conventions:
;                                   an assignment like `width: 10` is an object property
;                                   (group 5), only `property int x` is a declaration
;                                   (group 2). Both then demote an object's own state below
;                                   its logic. That reads fine on a visual Item and badly
;                                   on the two shapes SynQt actually has: a Source, whose
;                                   props ARE the contract and belong at the top, and a
;                                   client root, whose `visible/width/height` are what make
;                                   it a window at all. Neither moves the comment that
;                                   explains a property with it, so grouping strands them.
;                                   The conventions were written for scene objects; where
;                                   they fit, order by hand.
;   MaxColumnWidth=-1               No wrapping. qmlformat reflows expressions and no
;                                   setting stops it, so a limit only chooses where it goes
;                                   wrong: it breaks wherever the limit lands, between an
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
; So what is left is whitespace and semicolons: the parts with one right answer, that no
; reviewer should spend a comment on. Change any of it: it is your project's QML.

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
    id: window

    visible: true
    width: 360
    height: 240
    title: qsTr("SynQt app")

    // Surfaces the connection state to the browser console; a boot sentinel `synqt dev`
    // (and the browser end-to-end check) watch for. Invisible; harmless in the shipped app.
    Item {
        property string status: "state=" + Session.state
        onStatusChanged: console.log("SynQt client: " + status)
        Component.onCompleted: console.log("SynQt client booted")
    }

    Label {
        anchors.centerIn: parent
        // On one line on purpose: the scaffold ships with check.qml_format on, and
        // qmlformat reflows a wrapped expression, so a hand-wrapped ternary here would
        // report the new project as unformatted on its very first `synqt check`.
        text: Session.state === "connected" ? qsTr("Connected") : qsTr("Connecting...")
    }
}
"""


class NewProjectError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _config(name: str, origin_model: str, entities: List[Dict[str, Any]]) -> Dict[str, Any]:
    return {
        "project": {"name": name, "version": "0.1.0", "qt_version": QT_VERSION,
                    "origin_model": origin_model},
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
             origin_model: str = "same_origin", auth: Optional[str] = None,
             blueprints: Optional[List[str]] = None) -> str:
    root = Path(parent_dir) / name
    if root.exists() and any(root.iterdir()):
        raise NewProjectError(f"{root} already exists and is not empty")
    root.mkdir(parents=True, exist_ok=True)

    entities: List[Dict[str, Any]] = [
        {"name": "client", "kind": "client", "targets": ["wasm"]},
        {"name": "web", "kind": "service", "capability": "web_edge"},
    ]
    for blueprint in blueprints or []:
        entities.append({"name": blueprint, "kind": "service", "blueprint": blueprint})

    config = _config(name, origin_model, entities)
    if auth:
        # Mark the edge so the license generator knows it links Network Authorization.
        config["entities"][1]["identity"] = True
    (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

    for folder in ("client", "web", "shared"):
        (root / folder).mkdir(exist_ok=True)
    (root / "client" / "Main.qml").write_text(_MAIN_QML)

    (root / ".gitignore").write_text(
        "# SynQt: never commit mesh private keys, the toolchain cache, or build outputs\n"
        "build/\nsynqt/toolchain/\nsynqt/mesh/*.key\nsynqt/mesh/dev/\n.env\n")
    (root / ".env.example").write_text("# Entity secrets (env: references), never committed\n")
    _write_qmlformat_settings(root)

    presets.write(root, config)
    # The buildable app: the multi-binary CMakeLists and one main.cpp per entity, derived
    # from the topology so the scaffold compiles as-is (client + edge, no connect points).
    appgen.generate(root, config)

    lines = [
        f"Scaffolded '{name}' ({origin_model}). Next:",
        f"  cd {name} && synqt dev",
        "",
        licenses.CLIENT_GPL_WARNING,
    ]
    if auth:
        lines.insert(1, f"  Auth requested ({auth}): finish it with 'synqt add auth {auth}'.")
    return "\n".join(lines)
