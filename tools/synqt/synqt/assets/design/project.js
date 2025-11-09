// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The project a design document describes, rendered as text in the browser.
//
// This is for the hosted copy alone. Run locally there is a CLI behind the page: it works
// the change set out on disk with the same scaffolders `synqt add entity` and `synqt add
// contract` run, shows it as a diff, and writes nothing until somebody has read it. On
// synqt.org there is no disk and no CLI, so what the page offers instead is a download, and
// this is what fills it.
//
// It is a second writer, so it is held to the one job it can do reliably, a project that
// does not exist yet, rendered from the document alone. It never rewrites a synqt.yaml that
// is already there. The document models a topology and its contracts and nothing else, so
// rewriting a file that also holds scopes, security, TLS files and provider settings would
// quietly drop them; on a real project the original is on disk, and the server is what
// edits it.
//
// Every entity in the document has a directory here, and every directory has something in
// it: a client's is `Main.qml`, because the generated client main.cpp loads the QML module's
// `Main` and nothing else, and a service's holds the Source for each connect point it owns.
// An entity with no files would be an entity that is on the canvas, is in synqt.yaml, and
// cannot be found anywhere in the project it belongs to.
//
// Pure functions over the document, no DOM: the suite renders a project with node and hands
// it to `synqt check`, which is what stops this drifting from what `synqt new` writes.

import { declarationsFor } from "./source.js";

// The Qt this project pins, matching synqt/toolchain.py. The suite asserts the two agree,
// because a browser with no CLI behind it has nothing to ask.
const QT_VERSION = "6.11.1";

const CONTRACT_HEADER = "// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
    + "// SPDX-License-Identifier: Apache-2.0\n";

// A bare name goes in as it is. Anything else is quoted. JSON's string form is a YAML flow
// scalar, so quoting is one call rather than an escaping routine written here.
function scalar(value) {
    const text = String(value === undefined || value === null ? "" : value);
    return /^[A-Za-z_][A-Za-z0-9_.-]*$/.test(text) ? text : JSON.stringify(text);
}

function listing(values) {
    return `[${values.map(scalar).join(", ")}]`;
}

function entityLines(entity) {
    const lines = [`  - name: ${scalar(entity.name)}`,
                   `    kind: ${scalar(entity.kind || "service")}`];
    if (entity.capability) {
        lines.push(`    capability: ${scalar(entity.capability)}`);
    }
    if (entity.blueprint) {
        lines.push(`    blueprint: ${scalar(entity.blueprint)}`);
    }
    if (entity.identity) {
        lines.push("    identity: true");
    }
    if ((entity.targets || []).length) {
        lines.push(`    targets: ${listing(entity.targets)}`);
    }
    if (entity.provider) {
        lines.push("    provider:", `      name: ${scalar(entity.provider)}`);
    }
    // The same TLS block `synqt new` writes, pointing at the conventional place for the
    // certificate: `synqt build --release` and `synqt serve` refuse an edge that names
    // neither this nor a terminating proxy, so a downloaded project meets that rule from
    // its first release build rather than at the deployment.
    if (entity.capability === "web_edge") {
        lines.push("    tls:",
                   "      cert_file: certs/web/fullchain.pem",
                   "      key_file: certs/web/privkey.pem");
    }
    return lines;
}

function linkLines(link) {
    const lines = [`  - name: ${scalar(link.name)}`,
                   `    contract: ${scalar(link.contract)}`,
                   `    owner: ${scalar(link.owner)}`,
                   `    consumers: ${listing(link.consumers || [])}`,
                   `    instance: ${scalar(link.instance || "shared")}`];
    if (link.transport) {
        lines.push(`    transport: ${scalar(link.transport)}`);
    }
    return lines;
}

function block(name, items, render) {
    if (!items.length) {
        return [`${name}: []`];
    }
    return [`${name}:`, ...items.flatMap(render)];
}

// The synqt.yaml this document describes, in the shape and order `synqt new` writes it.
export function renderYaml(design) {
    return [
        "project:",
        `  name: ${scalar(design.project || "app")}`,
        "  version: 0.1.0",
        `  qt_version: ${QT_VERSION}`,
        "",
        "scopes:",
        "  order: [anonymous, user, moderator, admin]",
        "  hierarchical: true",
        "  default: anonymous",
        "",
        "security:",
        "  allowed_origins: [self]",
        "  cross_origin_isolation: false",
        "",
        "build:",
        "  client_threads: single",
        "",
        "check:",
        "  qml_format: true",
        "",
        ...block("entities", design.entities || [], entityLines),
        "",
        ...block("connect_points", design.links || [], linkLines),
        "",
    ].join("\n");
}

function params(list) {
    return (list || []).map((param) => `${param.type} ${param.name}`).join(", ");
}

function memberLine(member) {
    if (member.kind === "prop") {
        return `prop ${member.type} ${member.name}`;
    }
    if (member.kind === "model") {
        return `model ${member.name}(${params(member.roles)})`;
    }
    if (member.kind === "signal") {
        return `signal ${member.name}(${params(member.params)})`;
    }
    const returned = member.type ? `${member.type} ` : "";
    return `slot ${returned}${member.name}(${params(member.params)})`;
}

// Where the owner-side Source for a connect point lives when nothing says otherwise, and
// what goes in it. Both mirror addcontract.source_path and addcontract.source_stub, which
// is what the CLI writes for the same gesture; the suite asserts the two agree, because a
// download whose QML the CLI would not have written is a project that starts differing from
// itself the moment somebody runs `synqt design` on it.
export function sourcePath(owner, contract) {
    return `${owner}/${contract}.qml`;
}

export function sourceQml(contract, point, members) {
    const declared = declarationsFor(members);
    return `${CONTRACT_HEADER}
import QtQuick
import SynQt

// Owner of the "${point}" connect point. Its props, models and signals are the ones declared
// in shared/${contract}.syn, and nothing undeclared ever reaches a consumer. A slot a consumer
// calls arrives here with \`Caller\` set to whoever called it: authorize that caller first,
// then act. This file is where the rule lives; a check in a consumer's UI is a courtesy, not
// a guard.
${contract}Source {
    id: root
${declared ? "\n" + declared + "\n" : ""}}
`;
}

// The client's one entry point. The generated client main.cpp does
// `engine.loadFromModule(uri, "Main")`, so this file is the root object and its name is not a
// preference: a client whose window lives in a differently named file builds, loads, logs
// nothing and renders a blank page. It is the same file `synqt new` writes, and the suite
// asserts the two are byte for byte the same.
export function clientMain() {
    return `${CONTRACT_HEADER}
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    visible: true
    width: 360
    height: 240
    title: "SynQt app"

    // Surfaces the connection state to the browser console. A boot sentinel \`synqt dev\`
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
        // report the new project as unformatted on its very first \`synqt check\`.
        text: Session.state === "connected" ? "Connected" : "Connecting..."
    }
}
`;
}

// One `.syn` source, matching what designdoc.render_contract writes on the server side.
export function renderContract(name, members) {
    const lines = [CONTRACT_HEADER, `contract ${name} {`];
    for (const member of members || []) {
        lines.push(`    ${memberLine(member)}`);
    }
    lines.push("}");
    return `${lines.join("\n")}\n`;
}

// Where each entity's own QML lives, in the order the tree reads: the client's window first,
// then one Source per connect point the entity owns. A `qml` written on the link wins over
// the generated one, because that is what the editor stores when somebody types into the
// Source pane; the download then holds what they wrote rather than the stub it started from.
export function entityFiles(design, entity) {
    const files = [];
    if ((entity.kind || "service") === "client") {
        files.push({name: `${entity.name}/Main.qml`, text: entity.qml || clientMain(),
                    owner: entity.name});
        return files;
    }
    const seen = new Set();
    for (const link of design.links || []) {
        const contract = String(link.contract || "");
        if (link.owner !== entity.name || !contract) {
            continue;
        }
        const relative = link.server || sourcePath(entity.name, contract);
        if (seen.has(relative)) {
            continue;
        }
        seen.add(relative);
        files.push({name: relative,
                    text: link.qml || sourceQml(contract, link.name, link.members),
                    owner: entity.name, link: link.name});
    }
    return files;
}

// Every file the download holds, each under a directory named after the project: the
// configuration, one contract per link that names one, and the QML of every entity. A link
// whose contract has no members yet still gets both files, because the connect point already
// refers to them and an entity with a connect point and no Source for it does not start.
export function projectFiles(design) {
    const root = String(design.project || "app");
    const files = [{name: `${root}/synqt.yaml`, text: renderYaml(design)}];
    const written = new Set();
    for (const link of design.links || []) {
        const contract = String(link.contract || "");
        if (!contract || written.has(contract)) {
            continue;
        }
        written.add(contract);
        files.push({name: `${root}/shared/${contract}.syn`,
                    text: renderContract(contract, link.members)});
    }
    for (const entity of design.entities || []) {
        for (const file of entityFiles(design, entity)) {
            files.push({name: `${root}/${file.name}`, text: file.text,
                        owner: file.owner, link: file.link});
        }
    }
    return files;
}

export { QT_VERSION };
