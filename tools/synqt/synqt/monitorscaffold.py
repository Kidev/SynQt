# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Everything a monitor entity is made of, written in one go.

`synqt add entity ops --type monitor` writes:

* the monitor entity, which keeps the history and serves the console on its own port;
* a console client, marked `console: true`, delivered only to an operator;
* `monitoring.entity`, which makes every service report to it;
* a `bundles:` block whose anonymous entry is a static sign-in page, so a visitor without an
  operator session cannot download the console;
* the sign-in page.

The console QML is generic: it reads the framework `Console` contract, whose types are
strings, numbers and bools, so adding an entity does not rebuild it.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel

#: The loading-page palette, so the console matches the rest of SynQt.
BACKGROUND = "#0d1224"
SURFACE = "#161c33"
SURFACE_HIGH = "#1e2542"
BORDER = "#2b3358"
TEXT = "#d7dafa"
MUTED = "#8f96c4"
ACCENT = "#00a6ed"
GOOD = "#46f477"
WARN = "#e6b450"
BAD = "#ff6b6b"


def free_port(config: Dict[str, Any]) -> int:
    """A port no browser-facing entity in this project binds.

    The edge and the monitor both default to 8443. An entity that declares no port counts as
    binding the default.
    """
    taken = {appmodel.public_port(entity) for entity in appmodel.entities(config)
             if appmodel.serves_browser(entity)}
    port = appmodel.DEFAULT_PUBLIC_PORT
    while port in taken:
        port += 1
    return port


def monitor_block(name: str, config: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """The monitor entity's own entry in `synqt.yaml`."""
    return {
        "name": name,
        "type": "monitor",
        # Loopback by default: reaching the console should mean reaching the machine first.
        "public": {"host": "127.0.0.1", "port": free_port(config or {})},
        "retention": {"max_age_days": 14, "max_bytes": 512 * 1024 * 1024},
    }


def console_block(name: str, monitor: str) -> Dict[str, Any]:
    """The console client's entry: a client like any other, marked as the console."""
    return {
        "name": name,
        "type": "client",
        # A console is delivered by the monitor, gated on `operator`, and reaches no
        # application entity.
        "console": True,
        "edge": monitor,
    }


def bundles_block(console: str) -> Dict[str, str]:
    """Who may download what from the monitor port. An anonymous visitor gets the sign-in page
    (a different bundle, not a 403); the console needs a session holding `operator`.
    """
    return {"anonymous": "signin/", appmodel.MONITOR_SCOPE: console}


def signin_page(monitor: str) -> str:
    """The static page an anonymous visitor gets instead of the console. Plain HTML and one
    fetch: it posts to the monitor sign-in route and reloads, and the gate then serves the
    console.
    """
    return f"""<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{monitor} - sign in</title>
<style>
  :root {{ color-scheme: dark; }}
  body {{ margin: 0; min-height: 100vh; display: grid; place-items: center;
         background: linear-gradient(165deg, #201335 0%, #232a5c 38%, #0d1224 100%);
         color: {TEXT}; font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }}
  form {{ width: min(22rem, 90vw); padding: 2rem; border-radius: 14px;
          background: {SURFACE}; border: 1px solid {BORDER}; }}
  h1 {{ margin: 0 0 1.5rem; font-size: 1.15rem; font-weight: 600; }}
  label {{ display: block; margin-bottom: 1rem; color: {MUTED}; font-size: 0.85rem; }}
  input {{ display: block; width: 100%; margin-top: 0.35rem; padding: 0.6rem 0.7rem;
           box-sizing: border-box; border-radius: 8px; border: 1px solid {BORDER};
           background: {BACKGROUND}; color: {TEXT}; font: inherit; }}
  button {{ width: 100%; padding: 0.65rem; border: 0; border-radius: 8px;
            background: {ACCENT}; color: #04121c; font: inherit; font-weight: 600;
            cursor: pointer; }}
  p {{ margin: 1rem 0 0; min-height: 1.5em; color: {WARN}; font-size: 0.85rem; }}
</style>
</head>
<body>
<form id="signin">
  <h1>{monitor}</h1>
  <label>operator<input name="name" autocomplete="username" autofocus required></label>
  <label>password<input name="password" type="password"
                        autocomplete="current-password" required></label>
  <button type="submit">Sign in</button>
  <p id="said"></p>
</form>
<script>
  const form = document.getElementById("signin");
  const said = document.getElementById("said");
  form.addEventListener("submit", async (event) => {{
    event.preventDefault();
    said.textContent = "";
    const body = new URLSearchParams(new FormData(form));
    const answer = await fetch("/monitor/signin", {{
      method: "POST", body, credentials: "same-origin"
    }});
    if (answer.ok) {{
      // The session now holds `operator`, so the same URL is a different bundle.
      window.location.reload();
      return;
    }}
    // One message for every failure, so a guesser cannot tell which operator names exist.
    said.textContent = "That did not work.";
    form.password.value = "";
    form.password.focus();
  }});
</script>
</body>
</html>
"""


def console_qml(monitor: str) -> str:
    """The console: one page with the live tail, a filter and the health strip. It knows
    nothing about the system it watches (see the module docstring).
    """
    # The attached-handler name is the contract, `Console`, whatever the monitor is called
    # (`<Contract>.on<Signal>`, see synqtc's consumer output).
    accessor = appmodel.MONITOR_CONSOLE_CONTRACT
    return f'''// SPDX-FileCopyrightText: 2026 Alexandre \'kidev\' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitoring console: what every entity is doing now, what the monitor itself is
// doing, and a search.
//
// `Server` is the monitor, this client\'s edge; `{accessor}` is its contract, which attached
// signal handlers name. Every value arrives through the framework `Console` contract, so
// this file does not depend on the system it watches.
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {{
    id: window

    readonly property color accent: "{ACCENT}"
    readonly property color line: "{BORDER}"
    readonly property color muted: "{MUTED}"
    readonly property color surface: "{SURFACE}"
    readonly property color surfaceHigh: "{SURFACE_HIGH}"
    readonly property color textColor: "{TEXT}"

    function ask(): void {{
        Server.ask(search.text, entityFilter.text, severityFilter.currentValue, 500);
    }}

    function severityColor(severity: string): color {{
        if (severity === "error" || severity === "fatal") {{
            return "{BAD}";
        }}
        if (severity === "warning") {{
            return "{WARN}";
        }}
        return window.muted;
    }}

    color: "{BACKGROUND}"
    height: 800
    title: qsTr("SynQt monitor")
    visible: true
    width: 1280

    ColumnLayout {{
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {{
            Layout.fillWidth: true
            spacing: 12

            // The monitor's own counters. The pipeline drops rather than blocks, and
            // `dropped` shows whether a quiet period is a gap in the record.
            Repeater {{
                model: [
                    {{ "label": qsTr("received"), "value": Server.received, "warn": false }},
                    {{ "label": qsTr("stored"), "value": Server.stored, "warn": false }},
                    {{ "label": qsTr("dropped"), "value": Server.dropped, "warn": true }}
                ]

                delegate: Rectangle {{
                    id: counter

                    required property var modelData

                    border.color: window.line
                    border.width: 1
                    color: window.surface
                    implicitHeight: 64
                    implicitWidth: 150
                    radius: 10

                    ColumnLayout {{
                        anchors.centerIn: parent
                        spacing: 2

                        Label {{
                            color: window.muted
                            font.pixelSize: 12
                            text: counter.modelData.label
                        }}

                        Label {{
                            color: counter.modelData.warn && counter.modelData.value > 0
                                   ? "{WARN}" : window.textColor
                            font.pixelSize: 22
                            text: counter.modelData.value
                        }}
                    }}
                }}
            }}

            Item {{
                Layout.fillWidth: true
            }}

            // One row per entity the monitor has heard from, with when it was last heard,
            // so an entity that stopped reporting is visible.
            Repeater {{
                model: Server.entities

                delegate: Rectangle {{
                    id: health

                    required property string name
                    required property double events
                    required property double refusals
                    required property bool live

                    border.color: window.line
                    border.width: 1
                    color: window.surface
                    implicitHeight: 64
                    implicitWidth: 160
                    radius: 10

                    RowLayout {{
                        anchors.centerIn: parent
                        spacing: 8

                        Rectangle {{
                            color: health.live ? "{GOOD}" : "{BAD}"
                            height: 8
                            radius: 4
                            width: 8
                        }}

                        ColumnLayout {{
                            spacing: 2

                            Label {{
                                color: window.textColor
                                font.pixelSize: 14
                                text: health.name
                            }}

                            Label {{
                                color: window.muted
                                font.pixelSize: 11
                                text: qsTr("%1 events, %2 refused").arg(health.events)
                                                                   .arg(health.refusals)
                            }}
                        }}
                    }}
                }}
            }}
        }}

        RowLayout {{
            Layout.fillWidth: true
            spacing: 8

            TextField {{
                id: search

                Layout.fillWidth: true
                placeholderText: qsTr("search what was said, or paste a trace id")

                onAccepted: window.ask()
            }}

            // A text field, not a combo box: the entity list is a QAbstractItemModel, and a
            // combo box would need the Source to publish a second copy as an array.
            TextField {{
                id: entityFilter

                Layout.preferredWidth: 160
                placeholderText: qsTr("every entity")

                onAccepted: window.ask()
            }}

            ComboBox {{
                id: severityFilter

                model: [
                    {{ "text": qsTr("everything"), "value": "trace" }},
                    {{ "text": qsTr("info and worse"), "value": "info" }},
                    {{ "text": qsTr("warnings and worse"), "value": "warning" }},
                    {{ "text": qsTr("errors only"), "value": "error" }}
                ]
                textRole: "text"
                valueRole: "value"

                onActivated: window.ask()
            }}
        }}

        Rectangle {{
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: window.line
            border.width: 1
            color: window.surface
            radius: 10

            ListView {{
                id: tail

                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: Server.events

                ScrollBar.vertical: ScrollBar {{}}

                delegate: Rectangle {{
                    id: row

                    required property int index
                    required property double ts
                    required property string severity
                    required property string category
                    required property string entity
                    required property string message
                    required property double durationMs
                    required property string traceId

                    color: row.index % 2 === 0 ? "transparent" : window.surfaceHigh
                    height: 30
                    width: tail.width

                    RowLayout {{
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        Label {{
                            Layout.preferredWidth: 90
                            color: window.muted
                            font.family: "monospace"
                            font.pixelSize: 12
                            text: new Date(row.ts).toLocaleTimeString(Qt.locale(),
                                                                      "HH:mm:ss.zzz")
                        }}

                        Label {{
                            Layout.preferredWidth: 70
                            color: window.severityColor(row.severity)
                            font.pixelSize: 12
                            text: row.severity
                        }}

                        Label {{
                            Layout.preferredWidth: 100
                            color: window.accent
                            font.pixelSize: 12
                            text: row.entity
                        }}

                        Label {{
                            Layout.preferredWidth: 110
                            color: window.muted
                            font.pixelSize: 12
                            text: row.category
                        }}

                        Label {{
                            Layout.fillWidth: true
                            color: window.textColor
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            text: row.message
                        }}

                        Label {{
                            color: window.muted
                            font.pixelSize: 11
                            text: row.durationMs > 0 ? row.durationMs.toFixed(1) + " ms" : ""
                        }}

                        // Opens the whole trace, across every entity it touched.
                        Label {{
                            color: window.accent
                            font.pixelSize: 11
                            text: row.traceId.length > 0 ? qsTr("trace") : ""

                            TapHandler {{
                                onTapped: Server.follow(row.traceId)
                            }}
                        }}
                    }}
                }}
            }}
        }}
    }}

    // Shows why the last query was refused. The handler names the framework contract, so
    // this line is the same in every project.
    {accessor}.onRefused: reason => {{
        search.placeholderText = reason;
    }}
}}
'''


#: Where the monitor name goes in the templates :func:`design_asset` publishes. Only the
#: sign-in page has one; the console reads the `Console` contract and names nothing.
DESIGN_NAME_TOKEN = "__MONITOR_NAME__"


def design_asset() -> Dict[str, Any]:
    """Everything the hosted design editor needs to draw a monitor without a SynQt backend.

    The editor writes the same bytes the scaffolder would, and `tests/test_monitoring.py`
    fails when they diverge. The functions used are the ones :func:`scaffold` calls. The
    name is left as :data:`DESIGN_NAME_TOKEN` for the editor to substitute.
    """
    console = f"{DESIGN_NAME_TOKEN}-console"
    return {
        "name_token": DESIGN_NAME_TOKEN,
        "console_suffix": "-console",
        # free_port() beside one web edge with no port, as in the editor output.
        "port": free_port({"entities": [{"name": "web", "type": "web_edge"}]}),
        "retention": monitor_block(DESIGN_NAME_TOKEN)["retention"],
        "bundles": bundles_block(console),
        "console_block": console_block(console, DESIGN_NAME_TOKEN),
        "signin_html": signin_page(DESIGN_NAME_TOKEN),
        "console_qml": console_qml(DESIGN_NAME_TOKEN),
    }


def scaffold(project_dir: os.PathLike[str] | str, name: str) -> str:
    """Write the monitor, its console client, the gate, and `monitoring.entity`. All four or
    none.
    """
    # Local import: `addentity` imports this module.
    from . import addentity, appgen, presets, yamledit
    import yaml

    console = f"{name}-console"
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text()) or {}
    existing = {e.get("name") for e in (config.get("entities") or []) if isinstance(e, dict)}
    for taken in (name, console):
        if taken in existing:
            raise addentity.AddEntityError(f"an entity named '{taken}' already exists")

    monitor = monitor_block(name, config)
    monitor["bundles"] = bundles_block(console)
    if not config_path.exists():
        config_path.write_text("entities: []\n")

    # Spliced into the text, keeping the author's comments and formatting.
    text = config_path.read_text()
    text = yamledit.append_item(text, "entities", monitor)
    text = yamledit.append_item(text, "entities", console_block(console, name))
    config_path.write_text(text)

    # Written last, so an interrupted scaffold leaves no entity reporting to a missing
    # monitor.
    config = yaml.safe_load(config_path.read_text()) or {}
    if not appmodel.monitor_entity(config):
        config_path.write_text(config_path.read_text().rstrip("\n")
                               + f"\n\nmonitoring:\n  entity: {name}\n")

    monitor_dir = root / appmodel.entity_dir(monitor)
    monitor_dir.mkdir(parents=True, exist_ok=True)
    (monitor_dir / "signin").mkdir(exist_ok=True)
    (monitor_dir / "signin" / "index.html").write_text(signin_page(name))

    console_entity = console_block(console, name)
    console_dir = root / appmodel.entity_dir(console_entity)
    console_dir.mkdir(parents=True, exist_ok=True)
    (console_dir / "Main.qml").write_text(console_qml(name))

    config = yaml.safe_load(config_path.read_text()) or {}
    presets.write(root, config)
    appgen.generate(root, config)

    return "\n".join([
        f"Monitor '{name}' scaffolded, with its console client '{console}'.",
        "",
        f"  - {appmodel.entity_dir(monitor)}/ keeps the history and serves the console on",
        f"    127.0.0.1:{monitor['public']['port']}. It listens on loopback because a "
        "console that",
        "    shows every request a system has served is not something to expose by "
        "default;",
        "    reaching it should mean reaching the machine first, through a VPN or an SSH",
        "    tunnel.",
        f"  - {appmodel.entity_dir(console_entity)}/Main.qml is the console. It reads the",
        "    framework's own Console contract, so it does not change when your topology does.",
        f"  - {appmodel.entity_dir(monitor)}/signin/ is what an anonymous visitor gets. The",
        "    console bundle is not served at all until a session holds 'operator', so it is",
        "    not something to be found by guessing a URL.",
        "  - monitoring.entity was written, which is what makes every service report.",
        "",
        "  Next: create an operator.",
        f"    synqt monitor operator add <you>",
        "  and put the line it prints in the monitor's environment. Until you do, the",
        "  console refuses everybody, which is the right way round.",
    ])
