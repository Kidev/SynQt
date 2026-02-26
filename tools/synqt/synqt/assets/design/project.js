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
// quietly drop them. On a real project the original is on disk, and the server is what
// edits it.
//
// Every entity in the document has a directory here, and every directory has its own file
// from the moment the entity exists. A client's is `Main.qml`, because the generated client
// main.cpp loads the QML module's `Main` and nothing else, and every other entity's is a
// `pragma Shared` file named after it. A Source per owned connect point follows. An entity with
// no files would be an entity that is on the canvas, is in synqt.yaml, and cannot be found
// anywhere in the project it belongs to.
//
// Pure functions over the document, no DOM: the suite renders a project with node and hands
// it to `synqt check`, which is what stops this drifting from what `synqt new` writes.

import { withoutCommentary } from "./commentary.js";
import { declarationsFor, reroot, rootTypeSpan, withShared, withoutShared }
    from "./source.js";
import { entityType, isFront } from "./rules.js";
import { MONITOR_SCAFFOLD } from "./monitor.js";

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
                   `    type: ${scalar(entityType(entity))}`];
    if (entity.identity) {
        lines.push("    identity: true");
    }
    // The two a console client carries and nothing drawn on the canvas does. They are not
    // shades of configuration. `console` is what makes the monitor deliver this client
    // instead of the application's, and `edge` is which monitor delivers it. A console
    // written without them is an ordinary client sitting in a bundle map that promises an
    // operator console.
    if (entity.console) {
        lines.push("    console: true");
    }
    if (entity.edge) {
        lines.push(`    edge: ${scalar(entity.edge)}`);
    }
    if (!isShared(entity) && entityType(entity) !== "client") {
        lines.push("    shared: false");
    }
    if ((entity.targets || []).length) {
        lines.push(`    targets: ${listing(entity.targets)}`);
    }
    if (entity.provider) {
        lines.push("    provider:", `      name: ${scalar(entity.provider)}`);
    }
    // The same TLS block `synqt new` writes, pointing at the conventional place for the
    // certificate. `synqt build --release` and `synqt serve` refuse an edge that names
    // neither this nor a terminating proxy, so a downloaded project meets that rule from
    // its first release build rather than at the deployment.
    if (isWebEdge(entity)) {
        lines.push("    tls:",
                   "      cert_file: certs/web/fullchain.pem",
                   "      key_file: certs/web/privkey.pem");
    }
    // Everything else a monitor is, taken from the scaffolder rather than restated here
    // (monitor.js, generated from synqt/monitorscaffold.py). Loopback, because a console
    // that shows every request a system has served is not something to put on a public
    // interface because nobody chose otherwise, and a retention bound, because a store
    // without one fills the disk of the machine it is watching.
    if (entityType(entity) === "monitor") {
        lines.push("    public:",
                   "      host: 127.0.0.1",
                   `      port: ${MONITOR_SCAFFOLD.port}`,
                   "    retention:",
                   `      max_age_days: ${MONITOR_SCAFFOLD.retention.max_age_days}`,
                   `      max_bytes: ${MONITOR_SCAFFOLD.retention.max_bytes}`);
    }
    // Which scope is served which bundle, which is the delivery gate. A caller is served
    // the bundle their scope maps to and no file of any other, so an unauthorized visitor
    // does not have the privileged bundle on their disk to read. A monitor's is the
    // scaffolder's, because hiding a console behind a sign-in page is not a choice anybody
    // makes per project. Every other entity writes what the drawing says.
    //
    // This wrote nothing at all until a project needed it, which the panel did not know:
    // it read `bundles` and showed the mapping, so an edge with a gate displayed one here
    // and downloaded without one, and the gate is the whole point of having drawn it.
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

// One of the scaffolder's templates, with the name of the monitor somebody drew in
// place of the token it was published with.
function forMonitor(text, name) {
    return String(text).split(MONITOR_SCAFFOLD.name_token).join(String(name || ""));
}

// The console client a drawn monitor implies. A client like any other, marked as the
// console, whose edge is the monitor. It is derived rather than drawn, the same way
// `monitoring.entity` below is, because what somebody puts on the canvas is one monitor and
// what a monitor is made of is four things. `synqt design` reaches the same place from the
// other end. There the real scaffolder writes it on Apply, and it is a node on the canvas
// the next time the project is read.
export function consoleFor(entity) {
    return Object.fromEntries(
        Object.entries(MONITOR_SCAFFOLD.console_block)
            .map(([key, value]) => [key, typeof value === "string"
                ? forMonitor(value, entity.name) : value]));
}

function monitors(design) {
    return (design.entities || []).filter((entity) => entityType(entity) === "monitor");
}

// Every entity the project holds. The ones somebody drew, and the console each monitor
// implies that the drawing does not already carry.
//
// That second clause is the whole of it. A configuration this writes can be read back, and
// reading it back turns the derived console into an ordinary drawn entity. Deriving it
// again from the monitor beside it would then write the same entity twice, and a project
// with two entities of one name does not build. `synqt design` reaches the same answer from
// the other end, and for the same reason (designplan._with_scaffolded_monitors).
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

// Whether there is one of this entity for everybody or one per caller, the same rule
// appmodel.is_shared applies. Shared unless it says otherwise, and never for a client,
// which is one browser.
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
    // A front hands each scope's callers to the entity that serves them. Written before the
    // export block so the two are not separated by it. This is who answers, and that is what
    // they answer with.
    // The key being written is what says this point is answered by entities behind it, and
    // what is under it says which. So the key goes on whenever the switch is on, empty
    // included: an empty block is a front nobody has wired yet, which `synqt check` warns
    // about. Written only when something was under it, the editor drew a wedge with scope
    // seats and handed over a file that said plain edge, and the first thing the reader did
    // after the download was undo the switch they had thrown.
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
        ...block("entities", allEntities(design), entityLines),
        "",
        ...block("connect_points", design.links || [],
                 (link) => linkLines(design, link)),
        "",
        // Written last, where `synqt add entity --type monitor` writes it, and only when
        // there is a monitor to name. It is the whole wiring. The link every service opens
        // is derived from this line rather than declared, so a project with a monitor and
        // without it has an entity nothing reports to.
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

// The folder entities of each type sit in, the same table appmodel.TYPE_FOLDERS holds. An
// entity's own folder is that one, then its name. Everything the entity is made of lives in
// there and nowhere else, which is what lets a `.qml` dropped beside it be imported with no
// wiring at all.
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

// The type a connect point exports: its owner, capitalized, the same rule
// appmodel.contract_of applies. Nothing names it separately, because the owner names the
// point, and nothing carries a suffix, because an entity is one file called after itself.
export function contractOf(link) {
    const owner = String((link || {}).owner || "");
    return owner ? `${owner[0].toUpperCase()}${owner.slice(1)}` : "";
}

// What a link is called: the two entities it runs between, in the names their author gave
// them. It used to be the owner's name capitalised, which is the type a consumer writes in
// QML and not a name this drawing has anything to do with -- so a canvas of entities called
// `edge` and `app` labelled the line between them `Edge`, and a reader had two spellings of
// one thing and nothing saying they were one thing.
//
// This is the form for the places that take text and nothing else: a `title` attribute, a
// label read out by a screen reader. On the page itself a link is drawn rather than written,
// by `linkTitleNode`, with the arrow between the names as an arrow.
//
// `consumer` narrows it to one line. Without it the point is named by every consumer it has.
//
// The two ends are answered separately as well, because most places that name a link draw it
// rather than write it: the arrow between the two names is a mark on the page, and the names
// themselves are coloured for the role each end plays. This is the one reading of a link's
// name, so the drawn form and the written form can never say different things.
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

// Where the owner-side Source and the contract of a connect point live when nothing says
// otherwise, and what goes in the Source. All three mirror appmodel.source_path,
// appmodel.contract_path and addcontract.source_stub, which is what the CLI writes for the
// same gesture. The suite asserts the two agree, because a download whose QML the CLI would
// not have written is a project that starts differing from itself the moment somebody runs
// `synqt design` on it.
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

// The client's one entry point. The generated client main.cpp does
// `engine.loadFromModule(uri, "Main")`, so this file is the root object and its name is not a
// preference: a client whose window lives in a differently named file builds, loads, logs
// nothing and renders a blank page. It is the same file `synqt new` writes, and the suite
// asserts the two are byte for byte the same.
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

// Where each entity's own QML lives, in the order the tree reads: its own file first, then
// whatever else its type gives it. A `qml` written on the entity or the link wins over the
// generated one, because that is what the editor stores when somebody types into the pane. The
// download then holds what they wrote rather than the stub it started from.
//
// Every entity has its own file from the moment it exists, before it owns or consumes
// anything. An entity that is on the canvas and in synqt.yaml with an empty directory beside it
// is an entity nobody can open.
export function entityFiles(design, entity) {
    const link = (design.links || []).find(
        (one) => one.owner === entity.name && contractOf(one));
    const files = [];
    if (entityType(entity) === "monitor") {
        // No file of its own, and the scaffolder writes none either. What a monitor does is
        // the framework's, down to the connect point it owns, so there is nothing here for
        // an author to have written. What it does have is the gate an anonymous visitor is
        // handed instead of the console.
        return [{name: `${entityDir(entity)}/signin/index.html`, owner: entity.name,
                 text: forMonitor(MONITOR_SCAFFOLD.signin_html, entity.name)}];
    }
    if (entity.console) {
        // The console client, which is a client whose window nobody writes: it reads the
        // framework's own `Console` contract, so it is the same three hundred lines for
        // every project and the scaffolder is what has them (monitor.js).
        return [{name: entityQmlPath(entity), owner: entity.name,
                 text: MONITOR_SCAFFOLD.console_qml}];
    }
    if (!link) {
        files.push({name: entityQmlPath(entity), own: true, owner: entity.name,
                    text: withoutAPoint(entity, entity.qml)});
    } else {
        // One file: the entity is what it exports. The text somebody declared into on the
        // entity is the same text the link's Source shows, so it is written once, at the one
        // path, and the entity's copy wins because that is where the panel writes.
        const relative = link.server || sourcePath(entity, contractOf(link));
        const written = entity.qml || link.qml;
        files.push({name: relative, own: true, owner: entity.name, link: link.owner,
                    text: written
                        ? withAPoint(written, contractOf(link))
                        : sourceQml(contractOf(link), link.owner, link.members)});
    }
    if (entityType(entity) === "relational") {
        // The table the entity's own QML queries. `synqt add entity` writes one beside every
        // relational entity, and a project downloaded without it is a project whose first
        // `Db.query` finds no table.
        files.push({name: `${entityDir(entity)}/schema.sql`, owner: entity.name,
                    text: entity.schema || schemaSql()});
    }
    return files;
}


// An entity's own file once it exports nothing. What its author already wrote, kept.
//
// Only two things change, and neither is theirs. `pragma Shared` goes on, because there is
// one of an entity and this file is now the entity rather than a caller's surface. The root
// keeps the entity's own name. `synqt build` retypes a self-named root that resolves to
// nothing (synqt/qmlrewrite.py), so `Store { }` loads either way and the file goes on
// reading as the thing it is.
//
// Handing back the stub would throw away every property, function and signal in
// the file and leave an entity that no longer even imports SynQt. Deleting a line on the
// canvas is not permission to empty a file.
function withoutAPoint(entity, written) {
    if (!written) {
        return entityQml(entity);
    }
    if (entityType(entity) === "client") {
        return written;                      // a window is not one of anything
    }
    return withShared(written);
}

// The same file once it exports a connect point. The Source of that point.
//
// The pragma comes off, because the file is now the point's Source and the entity's own
// state moves to a shared file beside it. A root left at the scaffold's `QtObject` is
// retyped to the contract, which is the one root a Source can have. A root the author wrote
// themselves is left exactly as it is, and `synqt check` is what has an opinion about it.
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

// The file an entity *is*, as opposed to the connect points it exposes. A client's is the
// window. Every other entity's is a `pragma Shared` file named after it, which is where state
// that belongs to the whole entity goes and what its Sources reach for it by name.
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

// An entity's own QML. Shared because there is one of this entity. Its Sources may be
// created per session or per peer, and anything they share has to outlive any one of them.
// `synqt build` finds it by its `pragma Shared` and registers it under the entity's own QML
// module, so `${Name}.something` resolves inside every Source this entity owns. The copy the
// engine loads gets QML's own `pragma Singleton` written into it (synqt/qmlrewrite.py).
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

// Every file the download holds, each under a directory named after the project: the
// configuration, which carries what crosses every link, and the QML of every entity. A link
// with nothing on it yet still gets its Source, because the connect point already refers to
// it and an entity with a connect point and no Source for it does not start.
export function projectFiles(design) {
    const root = String(design.project || "app");
    const files = [{name: `${root}/synqt.yaml`, text: renderYaml(design)}];
    for (const entity of allEntities(design)) {
        for (const file of entityFiles(design, entity)) {
            files.push({name: `${root}/${file.name}`, text: file.text,
                        owner: file.owner, link: file.link, own: file.own});
        }
    }
    return files;
}

export { QT_VERSION };
