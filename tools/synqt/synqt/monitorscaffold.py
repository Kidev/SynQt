# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Everything a monitor entity is made of, written in one go.

A monitor is not one entity but three things that only work together. The entity that keeps
the history, a client that shows it, and the gate that decides who may download that client.
Scaffolding one of them and leaving the author to wire the other two would be scaffolding
the easy part. What makes a console safe is the part that is easy to leave out.

So `synqt add entity ops --type monitor` writes:

* the monitor entity, which keeps the history and serves the console on its own port;
* a console client, marked `console: true`, delivered only to an operator;
* `monitoring.entity`, which is what makes every service report to it;
* a `bundles:` block whose anonymous entry is a static sign-in page, so a visitor with no
  operator session cannot download the console at all;
* the sign-in page itself.

The console's QML is generic. It reads the framework's own `Console` contract,
whose every type is a string, a number or a bool, so the same console serves an auction, an
arena, and a system nobody has written yet. A project that adds an entity does not rebuild
it.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel

#: The palette the loading page uses, so a console looks like the rest of SynQt rather than
#: like a different product bolted on.
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
    """A port no browser-facing entity in this project already has.

    A project gains its second browser-facing server the moment it gains a monitor: the
    edge serves the application, the monitor serves its console. Both would default to 8443
    and the first `synqt dev` afterwards would fail to bind one of them, so the scaffolder
    steps past what is taken rather than writing a collision for `synqt check` to report.
    """
    taken = {int(appmodel.public_settings(entity).get("port") or 0)
             for entity in appmodel.entities(config)
             if appmodel.serves_browser(entity)}
    port = 8443
    while port in taken:
        port += 1
    return port


def monitor_block(name: str, config: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """The monitor entity's own entry in `synqt.yaml`."""
    return {
        "name": name,
        "type": "monitor",
        # Loopback by default, and said out loud in the scaffold. A console that shows every
        # request a system has served is not something to put on a public interface because
        # nobody thought about it. Reaching it should mean reaching the machine first.
        "public": {"host": "127.0.0.1", "port": free_port(config or {})},
        "retention": {"max_age_days": 14, "max_bytes": 512 * 1024 * 1024},
    }


def console_block(name: str, monitor: str) -> Dict[str, Any]:
    """The console client's entry: a client like any other, marked as the console."""
    return {
        "name": name,
        "type": "client",
        # One word, because the difference is not a shade of configuration: a console is
        # delivered by the monitor, gated on `operator`, and reaches the application's own
        # entities not at all.
        "console": True,
        "edge": monitor,
    }


def bundles_block(console: str) -> Dict[str, str]:
    """Who may download what, from the monitor's own port.

    The whole gate. An anonymous visitor gets a sign-in page and nothing else. Not a 403 on
    the console, which would confirm it is there, but a different bundle entirely. The
    console itself is only addressable once a session holds `operator`.
    """
    return {"anonymous": "signin/", appmodel.MONITOR_SCOPE: console}


def signin_page(monitor: str) -> str:
    """The static page an anonymous visitor gets instead of the console.

    Plain HTML and one fetch, because it is what a browser is handed before anything
    has been authenticated, so the less of it there is, the less there is to get wrong. It
    posts to the monitor's own sign-in route and reloads, at which point the delivery gate
    hands the same URL the console instead.
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
    // One message for every failure. Saying which half was wrong tells whoever is
    // guessing which operator names exist.
    said.textContent = "That did not work.";
    form.password.value = "";
    form.password.focus();
  }});
</script>
</body>
</html>
"""


def console_qml(monitor: str) -> str:
    """The console itself: one page, the live tail, a filter, and the health strip.

    Generic by construction. Everything it shows arrives through the framework's `Console`
    contract as a string, a number or a bool, so nothing here knows what the system it is
    watching is made of, and a project that adds an entity does not rebuild it.
    """
    # The attached-handler name is the CONTRACT, not the owner: `<Contract>.on<Signal>` is
    # what the consumer facade registers as a QML type (see synqtc's consumer output). For
    # the monitor's console point that is the framework's own `Console`, whatever the
    # monitor entity is called, which is the same reason this file does not change when the
    # topology does.
    accessor = appmodel.MONITOR_CONSOLE_CONTRACT
    return f'''// SPDX-FileCopyrightText: 2026 Alexandre \'kidev\' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitoring console. One page: what every entity is doing right now, what the monitor
// itself is doing, and a way to ask a different question.
//
// `Server` is the monitor, which is this client\'s edge; `{accessor}` is its contract, which
// is what an attached signal handler is written against. Every value shown arrives through
// the framework\'s own `Console` contract, so this file knows nothing about the system it
// watches.
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

            // What the monitor itself is doing. `dropped` is the honest half: the pipeline
            // drops rather than blocks, so a quiet period and a hole in the record look the
            // same until something says which it was.
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

            // One row per entity the monitor has heard from. An entity that has stopped
            // reporting is the case a monitor exists to notice, and silence is
            // indistinguishable from health without a row that says when it was last heard.
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

            // A field rather than a list of entities: the list lives in the `entities`
            // model, which is a QAbstractItemModel and not something QML can turn into a
            // combo box's array without the Source publishing a second copy of it.
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

                        // One click\'s whole story, across every entity it touched.
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

    // What the last question could not answer, said to the operator who asked it. The
    // attached handler names the contract, which is the framework\'s own and not this
    // monitor\'s, so this line is the same in every project.
    {accessor}.onRefused: reason => {{
        search.placeholderText = reason;
    }}
}}
'''


def scaffold(project_dir: os.PathLike[str] | str, name: str) -> str:
    """Write the monitor, its console client, the gate, and the key that wires it all up.

    All four or none. A monitor with no console is a store nobody reads. A console with no
    gate is every request the system has served, served to whoever finds the port, and
    neither is any use without `monitoring.entity`, which is the one line that makes the
    other entities report at all.
    """
    # Imported here rather than at module scope. `addentity` imports this module to route
    # `--type monitor` here, so importing it back at the top is a cycle.
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

    # Spliced into the text, like every other scaffold. The file is the author's, and one
    # added entity is not a reason to lose their comments and their formatting.
    text = config_path.read_text()
    text = yamledit.append_item(text, "entities", monitor)
    text = yamledit.append_item(text, "entities", console_block(console, name))
    config_path.write_text(text)

    # The one line that makes every service report. Written last, so a half-written scaffold
    # leaves a project that is merely missing a monitor rather than one whose entities are
    # all reporting to an entity that is not there.
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
