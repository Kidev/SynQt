// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The project a design document describes, rendered as text in the browser.
//
// For the hosted copy only. Run locally, the CLI behind the page works the change set out on
// disk with the scaffolders `synqt add entity` and `synqt add contract` run, shows it as a
// diff, and writes nothing until it is read. On synqt.org there is no disk and no CLI, so the
// page offers a download, and this fills it.
//
// It only renders a new project from the document. It never rewrites an existing synqt.yaml:
// the document models a topology and its contracts and nothing else, so rewriting a file
// that also holds scopes, security, TLS files and provider settings would drop them.
//
// Every entity has a directory and its own file from the moment it exists: `Main.qml` for a
// client, since the generated client main.cpp loads the module's `Main`, and a
// `pragma Shared` file named after any other entity. A Source per owned connect point
// follows.
//
// Pure functions over the document, no DOM: the suite renders a project with node and runs
// `synqt check` on it, which keeps this in step with what `synqt new` writes.

import { withoutCommentary } from "./commentary.js";
import { declarationsFor, reroot, rootTypeSpan, withShared, withoutShared }
    from "./source.js";
import { entityType, isFront, scopeDefaultOf, scopesOf } from "./rules.js";
import { MONITOR_SCAFFOLD } from "./monitor.js";

// The Qt this project pins, matching synqt/toolchain.py. The suite asserts the two agree,
// because a browser with no CLI behind it has nothing to ask.
const QT_VERSION = "6.12.0";

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
                   `    type: ${scalar(entityType(entity))}`];
    if (entity.identity) {
        lines.push("    identity: true");
    }
    // The two keys a console client carries: `console` makes the monitor deliver this client
    // instead of the application's, and `edge` names the monitor that delivers it.
    if (entity.console) {
        lines.push("    console: true");
    }
    if (entity.edge) {
        lines.push(`    edge: ${scalar(entity.edge)}`);
    }
    // A client's `shared` is written as drawn, as designdoc.py writes it, so the file says
    // what `synqt check` refuses rather than quietly losing it.
    if (entityType(entity) === "client") {
        if (typeof entity.shared === "boolean") {
            lines.push(`    shared: ${entity.shared}`);
        }
    } else if (!isShared(entity)) {
        lines.push("    shared: false");
    }
    if ((entity.targets || []).length) {
        lines.push(`    targets: ${listing(entity.targets)}`);
    }
    if (entity.provider) {
        lines.push("    provider:", `      name: ${scalar(entity.provider)}`);
    }
    // The TLS block `synqt new` writes, pointing at the conventional place for the
    // certificate, so a downloaded project passes the release TLS rule.
    if (isWebEdge(entity)) {
        lines.push("    tls:",
                   `      cert_file: ${scalar(`certs/${entity.name}/fullchain.pem`)}`,
                   `      key_file: ${scalar(`certs/${entity.name}/privkey.pem`)}`);
    }
    // The rest of a monitor comes from the scaffolder (monitor.js, generated from
    // synqt/monitorscaffold.py): the console on loopback, and a retention bound so the
    // store cannot fill the disk.
    if (entityType(entity) === "monitor") {
        lines.push("    public:",
                   "      host: 127.0.0.1",
                   `      port: ${MONITOR_SCAFFOLD.port}`,
                   "    retention:",
                   `      max_age_days: ${MONITOR_SCAFFOLD.retention.max_age_days}`,
                   `      max_bytes: ${MONITOR_SCAFFOLD.retention.max_bytes}`);
    }
    // Which scope is served which bundle, the delivery gate: a caller gets only the bundle
    // their scope maps to. A monitor's is the scaffolder's; every other entity writes what
    // the drawing says.
    const bundles = Object.entries(bundlesOf(entity));
    if (bundles.length) {
        lines.push("    bundles:");
        for (const [scope, bundle] of bundles) {
            lines.push(`      ${scope}: ${scalar(bundle)}`);
        }
    }
    return lines;
}

// The bundle map an entity is written with.
export function bundlesOf(entity) {
    if (entityType(entity) === "monitor") {
        return Object.fromEntries(Object.entries(MONITOR_SCAFFOLD.bundles)
            .map(([scope, bundle]) => [scope, forMonitor(bundle, entity.name)]));
    }
    return entity.bundles && typeof entity.bundles === "object" ? entity.bundles : {};
}

// One of the scaffolder's templates, with the drawn monitor's name in place of the token.
function forMonitor(text, name) {
    return String(text).split(MONITOR_SCAFFOLD.name_token).join(String(name || ""));
}

// The console client a drawn monitor implies: a client marked as the console, whose edge is
// the monitor. Derived, as `monitoring.entity` is. Under `synqt design` the scaffolder writes
// it on Apply, and it is a node on the canvas the next time the project is read.
export function consoleFor(entity) {
    return Object.fromEntries(
        Object.entries(MONITOR_SCAFFOLD.console_block)
            .map(([key, value]) => [key, typeof value === "string"
                ? forMonitor(value, entity.name) : value]));
}

function monitors(design) {
    return (design.entities || []).filter((entity) => entityType(entity) === "monitor");
}

// Every entity the project holds: the drawn ones, and the console each monitor implies that
// the drawing does not already carry. A configuration this writes can be read back, which
// turns the console into a drawn entity, and deriving it again would write it twice
// (designplan._with_scaffolded_monitors does the same).
function allEntities(design) {
    const drawn = (design.entities || []);
    const taken = new Set(drawn.map((entity) => String(entity.name || "")));
    return [...drawn, ...monitors(design).map(consoleFor)
        .filter((entity) => !taken.has(entity.name))];
}

// The monitor every service reports to, or "" for a project with none. The first one drawn:
// `monitoring.entity` names a single entity, and rules.js paints every monitor after it.
function monitorName(design) {
    const monitor = (design.entities || []).find(
        (entity) => entityType(entity) === "monitor");
    return monitor ? String(monitor.name || "") : "";
}

function isWebEdge(entity) {
    return entityType(entity || {}) === "web_edge";
}

// Whether there is one of this entity for everybody or one per caller, as appmodel.is_shared
// decides: shared unless it says otherwise, and never a client.
export function isShared(entity) {
    if (entityType(entity || {}) === "client") {
        return false;
    }
    return typeof (entity || {}).shared === "boolean" ? entity.shared : true;
}

function linkLines(design, link) {
    const lines = [`  - owner: ${scalar(link.owner)}`,
                   `    consumers: ${listing(link.consumers || [])}`];
    if (link.transport) {
        lines.push(`    transport: ${scalar(link.transport)}`);
    }
    // The scope a browser needs before it acquires the point at all, and the default for
    // every member of the block below that does not name one of its own.
    if (link.scope) {
        lines.push(`    scope: ${scalar(link.scope)}`);
    }
    // A front hands each scope's callers to the entity that serves them, written before the
    // export block. The key is written whenever the switch is on, empty included: an empty
    // block is a front nobody has wired yet, which `synqt check` warns about.
    if (isFront(link)) {
        const tiers = link.behind || {};
        const scopes = Object.keys(tiers).filter((scope) => tiers[scope]);
        lines.push(scopes.length ? "    behind:" : "    behind: {}");
        for (const scope of scopes) {
            lines.push(`      ${scope}: ${scalar(tiers[scope])}`);
        }
    }
    // What crosses the point, written on the point. The same block designdoc.render_export
    // writes on the server side, as a YAML literal so it reads as the lines it is.
    const members = link.members || [];
    if (members.length) {
        lines.push("    export: |");
        for (const member of members) {
            lines.push(`      ${memberLine(member)}`);
        }
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
        // The project's own scopes where it has any (the arena tutorial gates on `player`).
        `  order: ${listing(scopesOf(design))}`,
        "  hierarchical: true",
        // The scope a caller with no session holds, as the document carries it
        // (designdoc.scope_default_of).
        `  default: ${scalar(scopeDefaultOf(design))}`,
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
        ...block("entities", allEntities(design), entityLines),
        "",
        ...block("connect_points", design.links || [],
                 (link) => linkLines(design, link)),
        "",
        // Written last, where `synqt add entity --type monitor` writes it, when there is a
        // monitor. Every service's link to the monitor is derived from this line.
        ...(monitorName(design)
            ? ["monitoring:", `  entity: ${scalar(monitorName(design))}`, ""]
            : []),
    ].join("\n");
}

function params(list) {
    return (list || []).map((param) => `${param.type} ${param.name}`).join(", ");
}

function memberLine(member) {
    // The scope gate goes in front of whatever the member is, and a member with none
    // inherits the point's own `scope:`, so an empty one writes nothing at all.
    const gate = member.scope ? `<${member.scope}> ` : "";
    if (member.kind === "prop") {
        return `${gate}prop ${member.type} ${member.name}`;
    }
    if (member.kind === "model") {
        return `${gate}model ${member.name}(${params(member.roles)})`;
    }
    if (member.kind === "signal") {
        return `${gate}signal ${member.name}(${params(member.params)})`;
    }
    const returned = member.type ? `${member.type} ` : "";
    return `${gate}slot ${returned}${member.name}(${params(member.params)})`;
}

// The folder entities of each type sit in, as appmodel.TYPE_FOLDERS holds it. An entity's
// folder is that one, then its name, and everything the entity is made of lives there.
const TYPE_FOLDERS = {
    client: "client",
    web_edge: "web",
    relational: "db/relational",
    document: "db/document",
    cache: "cache",
    api: "api",
    jobs: "jobs",
    monitor: "monitor",
    service: "service",
};

// The type a connect point exports: its owner, capitalised, as appmodel.contract_of has it.
export function contractOf(link) {
    const owner = String((link || {}).owner || "");
    return owner ? `${owner[0].toUpperCase()}${owner.slice(1)}` : "";
}

// What a link is called: the two entities it runs between, by their own names. `linkTitle`
// is the text form, for a `title` attribute or a screen reader; on the page, `linkTitleNode`
// draws the same two ends with an arrow between them.
//
// `consumer` narrows it to one line. Without it the point is named by every consumer.
export function linkEnds(link, consumer) {
    const consumers = consumer ? [consumer] : ((link || {}).consumers || []);
    return {owner: (link || {}).owner || "nobody yet",
            consumers: consumers.join(", ") || "nobody yet"};
}

export function linkTitle(link, consumer) {
    const ends = linkEnds(link, consumer);
    return `${ends.owner} > ${ends.consumers}`;
}

export function entityDir(entity) {
    const folder = TYPE_FOLDERS[entityType(entity)] || TYPE_FOLDERS.service;
    return `${folder}/${entity.name}`;
}

// Where the owner-side Source of a connect point lives when nothing says otherwise, and what
// goes in it, mirroring appmodel.source_path and addcontract.source_stub. The suite asserts
// the two agree.
export function sourcePath(owner, contract) {
    return `${entityDir(owner)}/${contract}.qml`;
}

export function sourceQml(contract, point, members) {
    const declared = declarationsFor(members);
    return withoutCommentary(`${CONTRACT_HEADER}
import SynQt

// The connect point the "${point}" entity exports. What crosses it is the \`export:\` block
// on that point in synqt.yaml, and nothing undeclared ever reaches a consumer. A slot a
// consumer calls arrives here with \`Caller\` set to whoever called it: authorize that caller
// first, then act. This file is where the rule lives. A check in a consumer's UI is a
// courtesy, not a guard.
${contract} {
    id: root
${declared ? "\n" + declared + "\n" : ""}}
`);
}

// The client's entry point. The generated client main.cpp does
// `engine.loadFromModule(uri, "Main")`, so this file must be called Main.qml. The suite
// asserts it matches what `synqt new` writes, byte for byte.
export function clientMain() {
    return withoutCommentary(`${CONTRACT_HEADER}
import SynQt
import QtQuick.Controls

ApplicationWindow {
    id: root

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
`);
}

// Where each entity's QML lives, in the order the tree reads: its own file first, then
// whatever else its type gives it. A `qml` stored on the entity or the link (what was typed
// into the pane) wins over the generated stub.
export function entityFiles(design, entity) {
    const files = ownFiles(design, entity);
    // Every other file in the entity's folder, as the project on disk has it (the QML a
    // client's window opens, an edge's mapping hook). `companion` is the path inside the
    // folder, which an edit to one is stored under.
    const named = new Set(files.map((file) => file.name));
    for (const companion of entity.files || []) {
        const name = `${entityDir(entity)}/${companion.path}`;
        if (!named.has(name)) {
            files.push({name, owner: entity.name, companion: companion.path,
                        text: companion.text || ""});
        }
    }
    return files;
}

// The files an entity always has: its own QML or the Source of the point it exports, and a
// relational entity's table.
function ownFiles(design, entity) {
    const link = (design.links || []).find(
        (one) => one.owner === entity.name && contractOf(one));
    const files = [];
    if (entityType(entity) === "monitor") {
        // No QML of its own: the framework is the monitor. Its one file is the sign-in page
        // an anonymous visitor gets instead of the console.
        return [{name: `${entityDir(entity)}/signin/index.html`, owner: entity.name,
                 text: forMonitor(MONITOR_SCAFFOLD.signin_html, entity.name)}];
    }
    if (isFront(link)) {
        // A front owns a connect point it does not implement, so it has no Source: `synqt
        // check` would refuse an empty one beside a `behind:` block.
        return [];
    }
    if (entity.console) {
        // The console client's window reads the framework's `Console` contract, so it is the
        // same file in every project, from the scaffolder (monitor.js).
        return [{name: entityQmlPath(entity), owner: entity.name,
                 text: MONITOR_SCAFFOLD.console_qml}];
    }
    if (!link) {
        files.push({name: entityQmlPath(entity), own: true, owner: entity.name,
                    text: withoutAPoint(entity, entity.qml)});
    } else {
        // One file: the entity is what it exports. The entity's text wins over the link's,
        // since the panel writes to the entity.
        const relative = link.server || sourcePath(entity, contractOf(link));
        const written = entity.qml || link.qml;
        files.push({name: relative, own: true, owner: entity.name, link: link.owner,
                    text: written
                        ? withAPoint(written, contractOf(link))
                        : sourceQml(contractOf(link), link.owner, link.members)});
    }
    if (entityType(entity) === "relational") {
        // The table the entity's QML queries, as `synqt add entity` writes it.
        files.push({name: `${entityDir(entity)}/schema.sql`, owner: entity.name,
                    text: entity.schema || schemaSql()});
    }
    return files;
}


// An entity's own file once it exports nothing: what its author wrote, kept, with
// `pragma Shared` added, since the file is now the entity and not a caller's surface. The root
// keeps its name; `synqt build` retypes a self-named root (synqt/qmlrewrite.py). Removing a
// link on the canvas never empties a file.
function withoutAPoint(entity, written) {
    if (!written) {
        return entityQml(entity);
    }
    if (entityType(entity) === "client") {
        return written;                      // a window is not one of anything
    }
    return withShared(written);
}

// The same file once it exports a connect point, as that point's Source. The pragma comes
// off. A root left at the scaffold's `QtObject` is retyped to the contract; a root the author
// wrote is left alone for `synqt check` to judge.
function withAPoint(written, contract) {
    const span = rootTypeSpan(written);
    const root = span ? written.slice(span[0], span[1]) : "";
    const text = withoutShared(written);
    return root === "QtObject" ? reroot(text, contract) : text;
}

// The table a relational entity starts with, the same one `synqt add entity` writes.
export function schemaSql() {
    return "-- forward-only migrations, one statement per step\n"
        + "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        + "                    text TEXT NOT NULL, author TEXT NOT NULL);\n";
}

// A monitor's console window, which the pane opens when a monitor is selected.
export function consoleQmlPath(monitor) {
    return entityQmlPath(consoleFor(monitor));
}

// The file an entity is: a client's window, or a `pragma Shared` file named after any other
// entity, which holds the entity's state and which its Sources reach by name.
export function entityQmlPath(entity) {
    if (entityType(entity) === "client") {
        return `${entityDir(entity)}/Main.qml`;
    }
    return `${entityDir(entity)}/${capitalised(entity.name)}.qml`;
}

export function entityQml(entity) {
    if (entityType(entity) === "client") {
        return clientMain();
    }
    return entitySingleton(entity.name);
}

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}

// An entity's own QML, one per entity: its Sources may be per caller, and what they share
// outlives each of them. `synqt build` finds it by its `pragma Shared` and registers it, so
// `${Name}.something` resolves in every Source the entity owns; the copy the engine loads
// carries `pragma Singleton` (synqt/qmlrewrite.py).
export function entitySingleton(name) {
    const type = capitalised(name);
    return withoutCommentary(`${CONTRACT_HEADER}
pragma Shared

import SynQt

// The '${name}' entity itself: one of it, for as long as the entity runs, and one
// whatever the entity answers to \`shared:\`. State that belongs to the whole
// entity goes here rather than in a Source when the entity is not shared,
// because a Source is then one caller's and dies with them.
// Every Source this entity owns reaches it as \`${type}\`.
QtObject {
    id: root
}
`);
}

// Every file the download holds, under a directory named after the project: the
// configuration and every entity's files. A link with nothing on it yet still gets its
// Source, since an entity with a connect point and no Source does not start.
export function projectFiles(design) {
    const root = String(design.project || "app");
    const files = [{name: `${root}/synqt.yaml`, text: renderYaml(design)}];
    for (const entity of allEntities(design)) {
        for (const file of entityFiles(design, entity)) {
            files.push({name: `${root}/${file.name}`, text: file.text, owner: file.owner,
                        link: file.link, own: file.own, companion: file.companion});
        }
    }
    return files;
}

export { QT_VERSION };
