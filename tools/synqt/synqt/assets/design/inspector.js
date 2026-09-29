// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The panel for whatever is selected: an entity's own fields, or a connect point's, and
// under a connect point the members of the contract that crosses it.
//
// Every control writes straight into the document and asks the page to redraw. Two of them
// do more than they appear to. Renaming an entity carries the new name into every connect
// point that referred to the old one, and deleting one takes the connect points it owned
// with it, because leaving either behind would leave the project naming an entity that is
// not there.

import { behindOf, entityType, scopesOf } from "./rules.js";
import { ROLE_HELP, accessorName, codeLine, codeParts, codeWord, contractSvg,
         endsToOffer, glyphSvg, linkTitleNode, linksAreDerived, memberCode, memberMarkSvg,
         roleOf } from "./canvas.js";
import { linkTitle } from "./project.js";
import { baseType, declarations } from "./source.js";
import { contractBytes, memberSizeText, sizeText } from "./wire.js";

// The contract type vocabulary, from synqtc/types.py: QML's built-in value types, since a
// value is read from QML on one side and handed to QML on the other. No `float`: the contract
// compiler refuses it.
const TYPES = ["bool", "date", "double", "int", "list", "real", "string", "url", "var"];

// The types that take a bracketed size, and what the size counts (synqtc.types SIZED_TYPES).
const SIZED = {string: "characters", url: "characters", list: "elements", var: "bytes"};

const KINDS = ["prop", "model", "signal", "slot"];

// The word for each kind on the buttons that add one: the QML word, so a slot is `function`.
const KIND_WORDS = {prop: "property", model: "model", signal: "signal", slot: "function"};

// The entity types that take a data provider (addentity.TYPES).
const PROVIDER_FAMILIES = new Set(["relational", "cache", "document"]);

// Each entity type in one line, stated and not offered: an entity keeps the type of the
// palette row it was dragged from, since changing it would change what everything drawn
// against it means.
const KIND_LABELS = {
    client: "Client, built to WebAssembly and to a native desktop app",
    edge: "Web edge, the one entity facing the internet",
    relational: "Relational entity, a database behind a provider",
    cache: "Cache entity, a bounded store that forgets",
    document: "Document entity, records with no fixed columns",
    api: "API entity, where the system calls somebody else's",
    jobs: "Jobs entity, work on a timer with nothing listening",
    monitor: "Monitor entity, the operations record and its console",
    service: "Service entity, your own logic in its own binary",
};

const TARGETS = ["wasm", "desktop"];

function tag(name, attributes, text) {
    const node = document.createElement(name);
    for (const [key, value] of Object.entries(attributes || {})) {
        node.setAttribute(key, String(value));
    }
    if (text !== undefined) {
        node.textContent = text;
    }
    return node;
}

// A label and the one control it names. A real `<label>`, so clicking the words puts the
// caret in the box.
//
// `help` explains that one control, behind a mark beside the label.
function field(label, control, {role, help} = {}) {
    const wrap = tag("label", {class: "field"});
    const name = tag("span", {class: `field__label${role ? ` field__label--${role}` : ""}`},
                     label);
    asked(name, label, help);
    wrap.append(name, control);
    return wrap;
}

// A label over a group of controls: several checkboxes, a row of buttons, or a line of text
// that is stated rather than edited.
//
// Not a `<label>`: wrapped round several buttons or checkboxes, a `<label>` would give each
// of them the whole group's text as its accessible name.
function group(label, controls, {role, help} = {}) {
    const wrap = tag("div", {class: "field"});
    const name = tag("span", {class: `field__label${role ? ` field__label--${role}` : ""}`},
                     label);
    asked(name, label, help);
    wrap.append(name, controls);
    return wrap;
}

// A block of the panel with a heading and a rule above it. `help` says what the block is
// for; each setting explains itself.
function section(label, help, ...parts) {
    const box = tag("section", {class: "block"});
    blockHead(box, label, help);
    for (const part of parts) {
        if (part) {
            box.append(part);
        }
    }
    return box;
}

// The heading row of a block, and its `?`. Separate because two blocks are built a line at a
// time.
function blockHead(box, label, help) {
    const head = tag("div", {class: "block__head"});
    head.append(tag("h2", {class: "block__title"}, label));
    asked(head, label, help);
    box.append(head);
    return head;
}

// The `?` beside a label, and the words it holds.
//
// Hidden until the pointer is on its section; hovering the mark shows the words.
//
// `host` is what the mark is appended to. The tip is positioned against the ancestor that is
// the panel's width (`.field`, `.block__head`, `.helped`).
function asked(host, label, help) {
    const lines = (Array.isArray(help) ? help : [help]).filter(Boolean);
    if (!lines.length) {
        return host;
    }
    const ask = tag("button", {type: "button", class: "ask",
                               "aria-label": `What "${label}" means`}, "?");
    const tip = tag("span", {class: "ask__tip", role: "tooltip"});
    for (const line of lines) {
        tip.append(tag("p", {class: "ask__line"}, line));
    }
    host.append(ask, tip);
    return host;
}

// A control that is not a labelled field (a switch, a row of them) with a mark of its own,
// held at the end of the line.
function helped(node, label, help) {
    if (!help) {
        return node;
    }
    const wrap = tag("div", {class: "helped"});
    wrap.append(node);
    asked(wrap, label, help);
    return wrap;
}

// A line under whatever it explains. `loose` is for one after a heading or a button, not a
// field.
function note(text, loose) {
    return tag("p", {class: loose ? "field__note field__note--loose" : "field__note"}, text);
}

function text(value, onInput, placeholder) {
    const input = tag("input", {type: "text", placeholder: placeholder || ""});
    input.value = value || "";
    input.addEventListener("input", () => onInput(input.value));
    return input;
}

function choice(values, current, onChange, emptyLabel) {
    const node = tag("select");
    for (const value of values) {
        const option = tag("option", {value}, value === "" ? (emptyLabel || "none") : value);
        node.append(option);
    }
    node.value = current || "";
    node.addEventListener("change", () => onChange(node.value));
    return node;
}

// A switch and what it says. `code` sets a label that is a name from the project (an
// entity, a declaration) in the code face.
//
// `label` is a string, or nodes, such as a painted member of a contract.
function check(label, checked, onChange, {code, help} = {}) {
    const wrap = tag("label", {class: code ? "check check--code" : "check"});
    const box = tag("input", {type: "checkbox"});
    box.checked = checked;
    box.addEventListener("change", () => onChange(box.checked));
    const said = tag("span", {class: "check__label"});
    said.append(typeof label === "string" ? document.createTextNode(label) : label);
    wrap.append(box, said);
    return helped(wrap, typeof label === "string" ? label : "this", help);
}

// A type, and its size where it takes one: `string` and `[120]`, which the owner-side boundary
// enforces. The size box appears only for a sized type; empty means no limit.
//
// `allowed` is the vocabulary to offer, so a slot's return can add the empty option.
function typeAndSize(current, onChange, {allowed, emptyLabel} = {}) {
    const wrap = tag("span", {class: "typed"});
    const base = baseType(current);
    const written = String(current || "");
    const found = written.match(/\[(\d+)\]/);
    const size = found ? found[1] : "";

    const settle = (nextBase, nextSize) => {
        onChange(nextBase && nextSize && SIZED[nextBase]
            ? `${nextBase}[${nextSize}]` : nextBase);
    };
    wrap.append(choice(allowed || TYPES, base, (value) => settle(value, size), emptyLabel));
    if (SIZED[base]) {
        const box = tag("input", {type: "number", min: "1", class: "typed__size",
                                  placeholder: "no limit",
                                  title: `At most this many ${SIZED[base]}`});
        box.value = size;
        box.addEventListener("input", () => settle(base, box.value.trim()));
        wrap.append(box);
        wrap.append(tag("span", {class: "typed__unit"}, SIZED[base]));
    }
    return wrap;
}

function remover(title, onClick) {
    const button = tag("button", {type: "button", class: "icon-button", title}, "x");
    button.addEventListener("click", onClick);
    return button;
}

function adder(label, onClick) {
    const button = tag("button", {type: "button", class: "button"}, label);
    button.addEventListener("click", onClick);
    return button;
}

// What the panel currently has open, so a change of selection can close the rows that were
// opened on the last one.
let showing = "";

// The entity panel

// Rename an entity and everything that names it: the point it owns, every consumer list, and
// a front's routing. The panel and the canvas menu both rename through this.
export function renameEntity(design, entity, wanted) {
    const before = entity.name;
    entity.name = wanted;
    for (const link of design.links || []) {
        if (link.owner === before) {
            // A connect point is named by its owner, so its name moves with the owner's.
            link.owner = wanted;
            link.name = wanted;
            link.id = wanted;
        }
        link.consumers = (link.consumers || [])
            .map((consumer) => (consumer === before ? wanted : consumer));
        // A front routes each scope to an entity by name, so the rename is carried here too.
        for (const [scope, name] of Object.entries(link.behind || {})) {
            if (name === before) {
                link.behind[scope] = wanted;
            }
        }
    }
}

function entityPanel(design, entity, actions) {
    const role = roleOf(entity);
    const panel = document.createDocumentFragment();
    // The name and the glyph on one line, and under them what this kind of entity is for.
    const head = tag("div", {class: "inspector__head"});
    head.append(glyphSvg(role));
    head.append(tag("h2", {class: "inspector__title"}, entity.name || "this entity"));
    panel.append(head);
    panel.append(tag("p", {class: "inspector__help"}, ROLE_HELP[role]));

    // What it is called, and what it is, above how it behaves.
    panel.append(section("This entity", "",
        field("Name", text(entity.name, (value) => {
            renameEntity(design, entity, value);
            actions.rename("entity", value);
        }), {help: "The name the rest of the project reaches this by: its folder on disk, "
                   + "the accessor other entities write in their QML, and the connect point "
                   + "it owns. Typing a new one here carries it into all of them at once."}),
        // Fixed text: the kind was chosen when the entity was dragged off the palette.
        group("Kind", tag("p", {class: "field__fixed"}, KIND_LABELS[role]),
              {help: "What this entity is, chosen when it was dragged off the palette. It "
                     + "decides which Qt modules the entity links, where its files are "
                     + "written, and what it is allowed to reach. Drag a new entity out to "
                     + "have a different kind."})));

    const how = tag("div");
    if (entityType(entity) === "web_edge") {
        // Which scope is served which bundle, read-only: a value is a client entity or a
        // directory, and a text box could write a mapping `synqt check` refuses.
        const bundles = entity.bundles || {};
        const scopes = Object.keys(bundles).sort();
        const rows = tag("div");
        if (!scopes.length) {
            rows.append(tag("p", {class: "field__fixed"},
                             "one bundle, served to everybody"));
        }
        for (const scope of scopes) {
            const value = String(bundles[scope] || "");
            const kind = value.includes("/") ? "directory" : "client";
            rows.append(tag("p", {class: "field__fixed"},
                             `${scope} -> ${value} (${kind})`));
        }
        how.append(group("Bundles", rows,
                         {help: ["Which client bundle this edge serves each scope. A "
                                 + "caller is served the bundle their session's scope "
                                 + "maps to and no file of any other, so a privileged "
                                 + "bundle is not on an unauthorized visitor's disk at "
                                 + "all.",
                                 "A value with a `/` is a directory under this entity's "
                                 + "folder (a static gate); a bare name is a client "
                                 + "entity. With no bundles at all the project's one "
                                 + "client is served to everybody, which is what an app "
                                 + "that never wrote the key does."]}));
    }
    if (entityType(entity) === "client") {
        const targets = tag("div");
        for (const target of TARGETS) {
            targets.append(check(target, (entity.targets || []).includes(target),
                                 (on) => {
                const kept = new Set(entity.targets || []);
                if (on) {
                    kept.add(target);
                } else {
                    kept.delete(target);
                }
                entity.targets = TARGETS.filter((name) => kept.has(name));
                actions.changed();
            }, {code: true}));
        }
        how.append(group("Targets", targets,
                         {help: ["`wasm` builds the browser bundle the edge serves. "
                                 + "`desktop` builds a native app for Windows, macOS and "
                                 + "Linux from the same QML. Tick both to ship both out of "
                                 + "one source tree.",
                                 "Either way it is a connector: it reaches services "
                                 + "through the edge, and the secrets and the mesh "
                                 + "certificates stay on the entities behind it."]}));
    } else if (role === "edge") {
        how.append(check("Runs the sign-in flow", Boolean(entity.identity), (on) => {
            entity.identity = on;
            actions.changed();
        }, {help: "Turn this on for the entity that signs people in: it runs the OAuth "
                  + "exchange, keeps the tokens and the sessions, and hands the browser a "
                  + "cookie. `synqt add auth <provider>` fills in the rest."}));
        how.append(frontPanel(design, entity, actions));
    } else if (PROVIDER_FAMILIES.has(entityType(entity))) {
        how.append(field("Provider", text(entity.provider, (value) => {
            entity.provider = value;
            actions.changed();
        }, "sqlite"),
                         {help: ["The engine behind this kind of entity, named the way "
                                 + "`synqt providers` lists it: `sqlite` or `postgres` for a "
                                 + "relational entity, `memory` or `redis` for a cache. Type "
                                 + "another one in to swap engines; the connect point and "
                                 + "everything that consumes it stay as they are.",
                                 "The engine's credentials come from this entity's own "
                                 + "environment, so they stay on this side of the mesh."]}));
    }

    // A client has no instancing to choose. A `shared:` read from the file is offered for
    // removal, because `synqt check` refuses it whichever way it is written.
    if (role === "client" && typeof entity.shared === "boolean") {
        how.append(group("Shared", adder("Remove 'shared'", () => {
            delete entity.shared;
            actions.rebuild();
        }), {help: "A client is one browser and shares with nobody, so `shared:` says nothing "
                   + "here. To give every session a Source of its own, turn off \"One of it, "
                   + "for everybody\" on the edge."}));
    }

    // A monitor's Source and QML are the framework's, so it has no instancing to choose and
    // nothing to declare.
    const frameworks = linksAreDerived(entity);
    if (role !== "client" && !frameworks) {
        const shared = typeof entity.shared === "boolean" ? entity.shared : true;
        // `rebuild`, not `changed`: the rest of the panel depends on this switch.
        how.append(check("One of it, for everybody", shared, (on) => {
            entity.shared = on;
            actions.rebuild();
        }, {help: ["On, and one Source answers every caller while each of them still arrives "
                   + "with a `Caller` of their own. It is the default, and what a database or "
                   + "a cache wants: the state belongs to the system rather than to whoever "
                   + "asked.",
                   "Off, and every caller gets a Source holding only what is theirs, written "
                   + "as `shared: false` on this entity."]}));
    }

    if (how.childElementCount) {
        panel.append(section("How it runs",
                             "How this entity behaves once it is running, and what it is "
                             + "built to link. Everything here is written onto this entity "
                             + "in synqt.yaml.", how));
    }

    panel.append(wiredPanel(design, entity, actions));

    if (!frameworks) {
        panel.append(declaresPanel(design, entity, actions));
    }

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete entity");
    remove.addEventListener("click", () => actions.removeEntity(entity));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
}

// A name on this panel that is another entity, as a button that selects it.
function jump(name, onPick) {
    const button = tag("button", {type: "button", class: "button button--chip"}, name);
    button.addEventListener("click", () => onPick(name));
    return button;
}

function jumps(names, onPick) {
    const row = tag("div", {class: "chips"});
    for (const name of names) {
        row.append(jump(name, onPick));
    }
    return row;
}

// Who is at the other end of every line this entity is on: the consumers of the point it
// owns, and the owners of the points it consumes, in synqt.yaml's words. A point is named by
// its owner, so the owner is the name on the button.
function wiredPanel(design, entity, actions) {
    const links = design.links || [];
    const owned = links.filter((one) => one.owner === entity.name);
    const consumers = [...new Set(owned.flatMap((one) => one.consumers || []))];
    const owners = links.filter((one) => (one.consumers || []).includes(entity.name))
                        .map((one) => one.owner)
                        .filter(Boolean);
    const open = (name) => actions.select({kind: "entity", name});

    const about = "Who is at the other end of every line this entity is on. The same two "
        + "words synqt.yaml uses: an owner answers a connect point and decides what crosses "
        + "it, a consumer acquires a replica of it.";
    if (!owned.length && !owners.length && linksAreDerived(entity)) {
        return section("Wired to", about,
            note("Every entity. Each one reports to this monitor because monitoring.entity "
                 + "names it, and its console reaches it because that client is marked "
                 + "console: true. Both are configuration, so there is no line to draw.",
                 true));
    }
    if (!owned.length && !owners.length) {
        return section("Wired to", about,
            note("Nothing yet. Drag from a handle on this entity's rim to another entity to "
                 + "draw a connect point out of it, or from that entity to this one to "
                 + "consume theirs.", true));
    }

    const box = tag("div");
    if (owned.length) {
        const point = owned[0];
        box.append(group("Consumers", consumers.length
            ? jumps(consumers, open)
            : tag("p", {class: "field__fixed"}, "nobody yet"),
                         {help: consumers.length
                             ? [`Each of these acquires a replica of the connect point `
                                + `'${entity.name}' owns and can ask it for what crosses. `
                                + `This list is the authorization: an entity that is not on `
                                + `it is refused the replica.`,
                                "Press one to go to it."]
                             : [`'${entity.name}' owns a connect point and nothing consumes `
                                + `it yet. Drag a line from the contract icon on its rim to `
                                + `whatever should reach it.`]}));
        // Named as everything else names a link, at the size of the buttons above it.
        const contract = tag("button", {type: "button", class: "button button--chip"});
        contract.append(linkTitleNode(point));
        contract.title = `Open the connect point ${linkTitle(point)}`;
        contract.addEventListener("click", () => actions.openContract(point));
        box.append(group("Contract", contract,
                         {help: `The connect point '${entity.name}' owns, where you tick `
                                + `what crosses it out of what this entity declares. One `
                                + `entity owns one, and its consumers all get the same `
                                + `contract.`}));
    }
    if (owners.length) {
        // A client writes `Server` for the edge it reaches, the one accessor that is not the
        // owner's own name.
        const reaches = roleOf(entity) === "client" ? "Server" : accessorName(owners[0]);
        box.append(group("Owner", jumps([...new Set(owners)], open),
                         {help: owners.length === 1
                             ? [`'${entity.name}' consumes the connect point '${owners[0]}' `
                                + `owns, and reaches it by writing \`${reaches}\` in its own `
                                + `QML.`,
                                "Press it to go to that entity."]
                             : [`'${entity.name}' consumes a connect point from each of `
                                + `these, and reaches each one by that owner's own name in `
                                + `its QML.`,
                                "Press one to go to it."]}));
    }
    return section("Wired to", about, box);
}

// What the entity's own file declares, edited where it is declared.
//
// The same as writing the line into the file in the Files pane. This is the pool the entity's
// connect point ticks its contract from. It is read with the pane's parser and every control
// writes the line back, so the file stays the one record. Types and sizes are chosen from
// fixed lists.
//
// A model has no QML declaration form, so it is written straight onto the connect point
// this entity owns and listed with the other three kinds.
function declaresPanel(design, entity, actions) {
    const box = tag("div", {class: "block members"});
    blockHead(box, "What this entity declares",
              ["Everything this entity's own QML declares, read out of the file itself: a "
               + "property is state, a model is a list of rows, a signal is something that "
               + "happened, and a function is something callers can ask for.",
               "This is the pool every connect point this entity owns ticks its contract "
               + "from. Press a line to open it, and what you change is written back into "
               + "the file."]);
    const found = declarations(entity.qml || "");
    const point = ownPointOf(design, entity);
    const models = ((point || {}).members || []).filter((one) => one.kind === "model");
    if (!found.length && !models.length) {
        box.append(note("Nothing yet. A property is state, a model is a list of rows, a "
                        + "signal is something that happened, and a function is something "
                        + "callers can ask for.", true));
    }
    for (const member of found) {
        box.append(declaredPanel(design, entity, member, actions));
    }
    for (const model of models) {
        box.append(modelPanel(point, model, actions));
    }

    const adders = tag("div", {class: "members__add"});
    for (const kind of KINDS) {
        adders.append(adder(KIND_WORDS[kind], () => {
            if (kind === "model") {
                addModelTo(point, actions);
                return;
            }
            actions.declare(entity, {kind, name: "", type: kind === "prop" ? "int" : "",
                                     params: [], roles: []});
        }));
    }
    box.append(group("Add", adders,
                     {help: point || !models.length
                         ? ["A property, a signal and a function are written into this "
                            + "entity's own file, which is where a connect point it owns "
                            + "finds them.",
                            "A model is written straight onto the connect point, which is "
                            + "the one place a model can live: QML has a form for the other "
                            + "three and none for this."]
                         : ["A property, a signal and a function are written into this "
                            + "entity's own file.",
                            "A model lives on a connect point, so draw one off this entity "
                            + "first: drag from a handle on its rim to whatever should "
                            + "reach it."]}));
    return box;
}

// The connect point this entity owns, or null.
function ownPointOf(design, entity) {
    return (design.links || []).find((one) => one.owner === entity.name) || null;
}

function addModelTo(point, actions) {
    if (!point) {
        return;
    }
    point.members = point.members || [];
    point.members.push({kind: "model", name: "rows", type: "", params: [], roles: []});
    openWhenDrawn(point.owner, "rows");
    actions.rebuild();
}

// Which declarations are open for editing, by the key below.
//
// A closed row shows the declaration as its line of code (`property int highBid`); opening
// it offers the controls. Panel state, not project state: never written, and a reload starts
// closed.
const opened = new Set();

// The key: the declaration's line, which survives typing a new name into the open row (a key
// made of the name would close it mid-word).
function keyOf(entity, member) {
    return `${entity.name}\n${member.kind}\n${
        Number.isInteger(member.line) ? member.line : member.name}`;
}

// Rows opened on one selection are closed when the next is drawn, so no key collides.
function forgetOpen() {
    opened.clear();
    pending = "";
}

// The declaration a button just added, which opens when the rebuilt panel draws it. Named,
// not keyed by line, since the rewrite decides the line.
let pending = "";

export function openWhenDrawn(entityName, memberName) {
    pending = `${entityName}\n${memberName}`;
}

// Open a closed row, or close an open one, and build the panel again around it.
function toggleOpen(key, open, actions) {
    if (open) {
        opened.delete(key);
    } else {
        opened.add(key);
    }
    actions.rebuild();
}

// Whether this is the one that was added, and if so, spent.
function wasJustAdded(entityName, memberName) {
    if (pending !== `${entityName}\n${memberName}`) {
        return false;
    }
    pending = "";
    return true;
}

// One declaration the file holds. The line it is, and the controls over it once it is opened.
//
// The kind is stated, not offered: changing it would rewrite a line whose body, bindings and
// call sites belong to what it was.
function declaredPanel(design, entity, member, actions) {
    // A declaration with no name yet was just added, so it opens.
    const key = keyOf(entity, member);
    if (!member.name || wasJustAdded(entity.name, member.name)) {
        opened.add(key);
    }
    const open = opened.has(key);
    const box = tag("div", {class: `member${open ? " is-open" : ""}`});
    box.append(memberHead(member, open, () => toggleOpen(key, open, actions),
                          `Remove ${member.name || "this declaration"}`,
                          () => actions.undeclare(entity, member)));
    if (!open) {
        return box;
    }

    const edit = tag("div", {class: "member__edit"});
    const row = tag("div", {class: "member__row"});
    // Every control below writes through the captured member, so its name follows the box,
    // and a rename compares against the name the line had before this keystroke.
    row.append(field("Name", text(member.name, (value) => {
        const was = member.name;
        member.name = value;
        actions.redeclare(entity, member, {...member}, was);
    }, "name"),
                     {help: "What the declaration is called in the entity's own file, and "
                            + "what a consumer writes to reach it. Typing here rewrites the "
                            + "line in the file and carries the new name onto every contract "
                            + "already carrying it."}));

    // No size here or on the parameters: QML has no sized type. The size is set on the
    // connect point.
    if (member.kind === "prop") {
        row.append(field("Type", choice(TYPES, baseType(member.type) || "var", (value) => {
            member.type = value;
            actions.redeclare(entity, member, {...member}, member.name);
        }),
                         {help: "The QML value type this property holds, and what a consumer "
                                + "gets on the other side. The list is the vocabulary the "
                                + "contract compiler accepts, so every one of these builds."}));
    }
    if (member.kind === "slot") {
        row.append(field("Answers", choice(["", ...TYPES], baseType(member.type), (value) => {
            member.type = value;
            actions.redeclare(entity, member, {...member}, member.name);
        }, "nothing"),
                         {help: ["What the caller gets back. Give it a type and the call "
                                 + "resolves with a value, which a consumer awaits: "
                                 + "`Store.place(bid).then(ok => ...)`.",
                                 "Leave it at nothing and the call is made and not waited "
                                 + "on, which is what a fire and forget slot is."]}));
    }
    edit.append(row);

    if (member.kind === "signal" || member.kind === "slot") {
        const write = () => actions.redeclare(entity, member, member, member.name);
        edit.append(partsPanel(member, "params", "Parameters",
                               {changed: write, rebuild: write}, false));
    }
    box.append(edit);
    return box;
}

// A member's closed row: its kind, its line, and the remove button. The whole line opens it.
function memberHead(member, open, onToggle, removeTitle, onRemove) {
    const head = tag("div", {class: "member__head"});
    const summary = tag("button", {type: "button", class: "member__summary",
                                   "aria-expanded": String(open)});
    summary.append(memberMarkSvg(member.kind));
    summary.append(tag("span", {class: "member__kind code__tok code__tok--kw"},
                       KIND_WORDS[member.kind]));
    // The name on its own, so an unnamed one shows as such; the signature after it is
    // quieter.
    summary.append(member.name
        ? tag("span", {class: "member__name"}, member.name)
        : tag("span", {class: "member__name member__name--empty"}, "unnamed"));
    summary.append(signatureRest(member));
    summary.addEventListener("click", onToggle);
    head.append(summary);
    head.append(remover(removeTitle, onRemove));
    return head;
}

// What a declaration says after its name (its type, parameters, answer), written and painted
// as the file has it.
function signatureRest(member) {
    const rest = codeLine("member__signature");
    if (member.kind === "prop") {
        rest.append(codeWord("punct", ": "), codeWord("type", baseType(member.type) || "var"));
        return rest;
    }
    rest.append(codeWord("punct", "("));
    if (member.kind === "model") {
        codeParts(rest, member.roles);
        rest.append(codeWord("punct", ")"));
        return rest;
    }
    (member.params || []).forEach((param, index) => {
        if (index) {
            rest.append(codeWord("punct", ", "));
        }
        rest.append(codeWord("name", param.name || ""), codeWord("punct", ": "),
                    codeWord("type", baseType(param.type) || "var"));
    });
    rest.append(codeWord("punct", ")"));
    if (member.type) {
        rest.append(codeWord("punct", ": "), codeWord("type", baseType(member.type)));
    }
    return rest;
}

// A model, which lives on the point, not in the file. Only its roles cross; a row's other
// fields are dropped at the boundary.
function modelPanel(point, model, actions) {
    const key = `${point.owner}\nmodel\n${(point.members || []).indexOf(model)}`;
    if (!model.name || wasJustAdded(point.owner, model.name)) {
        opened.add(key);
    }
    const open = opened.has(key);
    const box = tag("div", {class: `member${open ? " is-open" : ""}`});
    box.append(memberHead(model, open, () => toggleOpen(key, open, actions),
                          `Remove ${model.name || "this model"}`, () => {
        point.members = (point.members || []).filter((one) => one !== model);
        actions.rebuild();
    }));
    if (!open) {
        return box;
    }
    const edit = tag("div", {class: "member__edit"});
    edit.append(field("Name", text(model.name, (value) => {
        model.name = value;
        actions.changed();
    }, "name"),
                      {help: "What the model is called on the contract. The owner publishes "
                             + "its rows by binding `<name>Rows` to wherever they live, and "
                             + "a consumer hands the same name to a view as its model."}));
    edit.append(partsPanel(model, "roles", "Roles", actions, true));
    box.append(edit);
    return box;
}

// The connect point panel

// The parameters of a signal or a function, or the roles of a model.
//
// `sized` says whether a size can be set: a size lives in the connect point's `export:` block,
// and a parameter read out of the owner's QML has nowhere to keep one
// (`function add(text: string[200])` is a syntax error).
function partsPanel(member, key, label, actions, sized) {
    const box = tag("div", {class: "member__parts"});
    box.append(tag("div", {class: "member__parts-title"}, label));
    const parts = member[key] || [];
    parts.forEach((part, index) => {
        const row = tag("div", {class: "member__part"});
        const onType = (value) => {
            part.type = value;
            actions.changed();
        };
        row.append(sized ? typeAndSize(part.type || "string", onType)
                         : choice(TYPES, baseType(part.type) || "string", onType));
        row.append(text(part.name, (value) => {
            part.name = value;
            actions.changed();
        }, "name"));
        row.append(remover(`Remove ${part.name || "this one"}`, () => {
            parts.splice(index, 1);
            actions.rebuild();
        }));
        box.append(row);
    });
    box.append(adder(`Add ${label.toLowerCase().replace(/s$/, "")}`, () => {
        member[key] = parts;
        parts.push({type: "string", name: ""});
        actions.rebuild();
    }));
    return box;
}

// The contract as a list to tick, out of what the owner entity declares, in the `export:`
// block's vocabulary. Only what the owner has is offered, so a member never reaches a
// contract before the file that implements it has the line. One list for every consumer.
function ticksPanel(design, link, actions) {
    const box = tag("div", {class: "block members"});
    const head = blockHead(box, "What crosses it",
              [`Everything ticked here is what '${link.owner || "the owner"}' says to `
               + `whoever consumes this point, and it is what the generated replica carries. `
               + `Nothing else ever crosses.`,
               "The list is what the owner entity declares, so a member reaches a consumer "
               + "because somebody ticked it here, and the file that implements it already "
               + "has the line."]);
    // And how much of it: a ceiling worked out from the sizes in the contract (wire.js),
    // never a measurement.
    head.append(wireSize(link));
    const owner = (design.entities || []).find((one) => one.name === link.owner);
    if (!owner) {
        box.append(note("No owner yet, so there is nothing to carry.", true));
        return box;
    }

    // Everything the owner declares, plus everything the point already carries that the
    // reader did not find in the file (a model, a property set from a binding, a signal raised
    // through `Caller`), each with a way to take it off.
    const declared = declarations(owner.qml || "");
    const carriedOnly = (link.members || [])
        .filter((one) => !declared.some((member) => member.name === one.name));
    const offered = [...declared, ...carriedOnly];
    const ticked = new Set((link.members || []).map((member) => member.name));
    if (!offered.length) {
        box.append(note(`'${link.owner}' declares nothing yet, so this contract is not `
                        + "finished. Add a property, a signal or a function on the entity "
                        + "and it will be here to tick.", true));
    }

    const list = tag("div", {class: "ticks"});
    for (const member of offered) {
        const row = tag("div", {class: "tick"});
        row.append(check(memberCode(member), ticked.has(member.name), (on) => {
            actions.tick(link, member, on);
        }, {code: true}));
        // What the point adds to a ticked member (a scope gate and, for a sized type, a
        // limit), offered only once it is on the contract, under the member.
        const carried = (link.members || []).find((one) => one.name === member.name);
        if (!carried) {
            list.append(row);
            continue;
        }
        // Shown as a line until pressed, which opens the controls.
        const key = `${link.owner}\ntick\n${member.name}`;
        const sized = carried.kind === "prop" && SIZED[baseType(carried.type)];
        if (opened.has(key)) {
            const extras = tag("div", {class: "tick__extras"});
            // The size the owner-side boundary enforces, kept on the contract since QML has no
            // sized type.
            if (sized) {
                extras.append(field("At most", typeAndSize(carried.type, (value) => {
                    carried.type = value;
                    actions.changed();
                }),
                                    {help: "The limit the owner-side boundary holds this "
                                           + "member to, written into the contract as "
                                           + "`string[120]`. It belongs here because QML has "
                                           + "no type with a size in it, so the declaration "
                                           + "in the file cannot carry one. Leave it empty "
                                           + "for no limit."}));
            }
            // A scope gate on this member alone.
            extras.append(field("Scope", choice(["", ...scopesOf(design)], carried.scope || "",
                                                (value) => {
                carried.scope = value;
                actions.changed();
            }, "the connect point's scope"),
                                {help: "The gate on this member alone. Raise it above the "
                                       + "point's own scope and this one member is held back "
                                       + "from callers the rest of the point answers, which "
                                       + "is how one admin slot lives on a public "
                                       + "connect point."}));
            row.append(extras);
        } else {
            row.append(tickSummary(carried, sized, link, () => {
                opened.add(key);
                actions.rebuild();
            }));
        }
        list.append(row);
    }
    box.append(list);
    if (carriedOnly.length) {
        box.append(note(`${carriedOnly.length === 1 ? "One member here is" : "Some of these"} `
                        + `${carriedOnly.length === 1 ? "carried" : "are carried"} by the `
                        + `contract and written in '${link.owner}' as code: a model lives on `
                        + `the point itself, and a property set from a binding or a signal `
                        + `raised through Caller is a line this reader takes at its word. `
                        + `Untick one to take it off the contract.`, true));
    }

    const consumers = link.consumers || [];
    if (!consumers.length) {
        box.append(note("Drag from the contract icon to an entity to say who gets this. "
                        + "A connect point exists before anything consumes it.", true));
    }
    return box;
}

// What the connect point adds to one ticked member, in a line: its gate, its limit and its
// size. A set value is coloured and a default is not. Pressing it offers the controls.
function tickSummary(carried, sized, link, onOpen) {
    const line = tag("button", {type: "button", class: "tick__summary",
                                title: "Set the scope and the limit for this member"});
    const gate = tag("span", {class: carried.scope ? "tick__set" : "tick__default"},
                     carried.scope || "the point's scope");
    line.append(gate);
    if (sized) {
        const found = String(carried.type || "").match(/\[(\d+)\]/);
        line.append(tag("span", {class: "tick__dot"}, "\u00b7"));
        line.append(tag("span", {class: found ? "tick__set" : "tick__default"},
                        found ? `at most ${found[1]}` : "no limit"));
    }
    line.append(tag("span", {class: "tick__dot"}, "\u00b7"));
    line.append(tag("span", {class: "tick__bytes"}, memberSizeText(carried, link)));
    line.addEventListener("click", onOpen);
    return line;
}

// What the whole contract costs: one crossing of each member, a model counted as one row. A
// contract with one unbounded member has no ceiling, and the chip says so.
function wireSize(link) {
    const cost = contractBytes(link);
    if (!(link.members || []).length) {
        return tag("span", {class: "block__bytes"}, "");
    }
    return tag("span", {class: `block__bytes${cost.bounded ? "" : " block__bytes--open"}`,
                        title: cost.bounded
                            ? `At most ${sizeText(cost.bytes)} crosses this link when every `
                              + "member crosses once and the model carries one row. Worked "
                              + "out from the sizes written into the contract, so it is a "
                              + "ceiling and not a measurement."
                            : "One member here has no limit written on it, so nothing bounds "
                              + `what crosses. The rest of the contract comes to `
                              + `${sizeText(cost.bytes)}; give the open member a size and `
                              + "this becomes a ceiling."},
               cost.bounded ? `\u2264 ${sizeText(cost.bytes)}` : `> ${sizeText(cost.bytes)}`);
}

// Whether this edge hands its callers on, and where each scope currently goes.
//
// On the edge's panel, beside "runs the sign-in flow", though synqt.yaml writes the flag on
// the point. Only the switch: the routing is drawn on the canvas by dragging between a seat
// and an entity.
function frontPanel(design, entity, actions) {
    const box = document.createDocumentFragment();
    const clients = new Set((design.entities || [])
        .filter((one) => entityType(one) === "client").map((one) => one.name));
    const link = (design.links || []).find((one) => one.owner === entity.name);
    if (!link || !(link.consumers || []).some((consumer) => clients.has(consumer))) {
        // Nothing to split yet. An edge no browser consumes has no callers to hand on.
        return box;
    }
    const tiers = behindOf(link);
    const isFront = Boolean(link.behind);
    const wired = scopesOf(design).filter((scope) => tiers[scope]);
    // Turned on, this edge stops answering its own connect point and every caller is served
    // by the entity their scope is wired to.
    box.append(check("Hands callers to the entities behind it", isFront, (on) => {
        link.behind = on ? {...tiers} : undefined;
        if (!on) {
            delete link.behind;
        }
        actions.rebuild();
    }, {help: isFront
        ? [`'${entity.name}' keeps the session and the sign-in, and each caller is served by `
           + `the entity wired to their scope. The browser goes on writing `
           + `\`${accessorName(entity.name)}\`, whoever answers behind it.`,
           `Every entity wired to a scope carries the members this point offers callers of `
           + `that scope; Review changes is what compares the two.`,
           "Drag between a scope on this edge's back and an entity to wire one, either way "
           + "round, or drag a seat onto empty canvas to take it off."]
        : [`'${entity.name}' answers its own connect point, which is the ordinary shape: one `
           + `edge, one Source, every caller.`,
           "Turn this on to make it a front. It keeps the session and the sign-in, and hands "
           + "each caller to the entity that serves people of their scope, which is how an "
           + "admin surface runs in its own binary."]}));
    if (!isFront) {
        return box;
    }
    // Where each scope currently goes, in words, since the seats on the canvas are small.
    box.append(note(wired.length
        ? `Drawn on the canvas: ${wired.map((scope) => `${scope} to '${tiers[scope]}'`)
            .join(", ")}.`
        : "No scope is wired yet, so this front hands nobody anywhere. Drag between a scope "
          + "on its back and the entity that serves it, either way round."));
    return box;
}

// The panel's opening line: what this connect point does, in the names of the entities at
// its ends.
function contractSays(design, link) {
    const consumers = link.consumers || [];
    const carries = (link.members || []).length;
    const browsers = consumers.filter((name) => (design.entities || [])
        .some((one) => one.name === name && entityType(one) === "client"));
    const mesh = link.transport === "local" ? "a local socket on this host"
                                            : "the mesh, over mutual TLS";
    const how = !browsers.length ? mesh
        : (browsers.length === consumers.length
            ? "the browser link, over TLS the edge terminates"
            : `the browser link for ${listed(browsers)} and ${mesh} for the rest`);
    if (!link.owner) {
        return "Nothing owns this connect point yet, so nothing answers it. Name the entity "
               + "that does below, and it becomes the name the point is reached by.";
    }
    if (!consumers.length) {
        return `'${link.owner}' owns this connect point and nothing consumes it yet. Drag `
               + `from its icon on the canvas to whatever should reach it, or tick that `
               + `entity below.`;
    }
    return `'${link.owner}' answers this connect point, and ${listed(consumers)} `
           + `${consumers.length === 1 ? "acquires" : "each acquire"} a replica of it over `
           + `${how}. ${carries ? `${carries} member${carries === 1 ? "" : "s"} of `
                                  + `'${link.owner}' cross${carries === 1 ? "es" : ""} it`
                                : "Nothing crosses it yet"}, and `
           + `${link.scope ? `a caller has to hold '${link.scope}' to reach it`
                           : "any caller may reach it"}.`;
}

// A list of names as a sentence says them, with quotation marks and an `and` at the end.
function listed(names) {
    const quoted = names.map((name) => `'${name}'`);
    if (quoted.length < 2) {
        return quoted.join("") || "nobody";
    }
    return `${quoted.slice(0, -1).join(", ")} and ${quoted[quoted.length - 1]}`;
}

// The connect point itself, opened by clicking its icon on the canvas: who owns it, who may
// consume it, its scope, how it is carried, and what crosses it.
function contractPanel(design, link, actions) {
    const panel = document.createDocumentFragment();
    // Opened as an entity's panel opens: its mark, its name, and one line saying what it does.
    const head = tag("div", {class: "inspector__head"});
    head.append(contractSvg());
    const title = tag("h2", {class: "inspector__title"});
    title.append(linkTitleNode(link));
    head.append(title);
    panel.append(head);
    panel.append(tag("p", {class: "inspector__help"}, contractSays(design, link)));

    // The owner names the point. Each list offers every entity that can be drawn at that end
    // and keeps whoever is already there (endsToOffer, as the canvas uses).
    const owners = endsToOffer(design.entities, link.owner);
    const names = endsToOffer(design.entities, link.consumers || []);
    const taken = new Set((design.links || [])
        .filter((one) => one !== link)
        .map((one) => one.owner));
    const who = tag("div");
    // The two labels take the role colours the canvas and the card use.
    who.append(field("Owner", choice(["", ...owners.filter((name) => !taken.has(name))],
                                     link.owner, (value) => {
        link.owner = value;
        link.id = value;
        link.name = value;
        link.consumers = (link.consumers || []).filter((consumer) => consumer !== value);
        actions.rebuild();
    }, "nobody yet"),
                     {role: "owner",
                      help: [`The entity that answers this connect point, hosts its Source `
                             + `and decides what crosses it.`,
                             `The owner is the name: a consumer reaches the point by writing `
                             + `\`${link.owner ? accessorName(link.owner) : "<Owner>"}\` in `
                             + `its QML, which is also the type the contract carries. The `
                             + `list offers each entity that has a connect point to spare, `
                             + `since one entity owns one.`]}));

    const consumers = tag("div");
    for (const name of names.filter((name) => name !== link.owner)) {
        consumers.append(check(name, (link.consumers || []).includes(name), (on) => {
            const kept = new Set(link.consumers || []);
            if (on) {
                kept.add(name);
            } else {
                kept.delete(name);
            }
            link.consumers = names.filter((entity) => kept.has(entity));
            // `rebuild`: who consumes a point decides whether the owner can be a front at
            // all, and whether gating a member on a scope means anything.
            actions.rebuild();
        }, {code: true}));
    }
    who.append(group("Consumers", consumers,
                     {role: "consumer",
                      help: ["Every entity ticked here acquires a replica of this connect "
                             + "point and can ask it for what crosses.",
                             "The list is the authorization. A service is checked against "
                             + "the verified name on its certificate; a browser reaches the "
                             + "point through the edge, under its session's scope."]}));
    panel.append(section("The two ends",
                         "Who answers this connect point, and who is allowed to reach it. "
                         + "Both are written on the point in synqt.yaml, and both are drawn "
                         + "on the canvas as the line between them.", who));

    const reach = tag("div");
    // `rebuild`, because this is the default every member below inherits and the list of
    // members says what each one is gated on.
    reach.append(field("Scope", choice(["", ...scopesOf(design)], link.scope || "", (value) => {
        link.scope = value;
        actions.rebuild();
    }, "any session, anonymous included"),
                       {help: ["The session a browser has to hold to acquire this connect "
                               + "point at all. A caller holding it gets the replica, its "
                               + "state and its slots; a caller below it is served the page "
                               + "and never the point.",
                               "It is also the default every member below inherits, so "
                               + "raising one member on its own is how an admin surface "
                               + "stays off a public page."]}));

    // `rebuild`, so the help on the transport follows the value at once.
    reach.append(field("Transport", choice(["", "local"], link.transport, (value) => {
        link.transport = value;
        actions.rebuild();
    }, "mutual TLS (the default)"),
                       {help: link.transport === "local"
                           ? ["A local socket on one host, opted into by name. It is the "
                              + "fast path: no TLS handshake, and the socket file is "
                              + "restricted to the user the entities run as.",
                              "The operating system identifies that user rather than the "
                              + "entity, so `Caller.entity` here is trusted by colocation. "
                              + "`synqt check` flags every link that takes it."]
                           : ["Mutual TLS on every link, loopback included: both ends verify "
                              + "the other against the project CA, and the verified subject "
                              + "on the peer certificate is the calling entity's name.",
                              "`local` swaps it for a same-host socket where the operating "
                              + "system identifies the user instead."]}));
    panel.append(section("Who may reach it",
                         "What a caller has to hold to acquire this point, and how the two "
                         + "ends carry it between them.", reach));

    panel.append(ticksPanel(design, link, actions));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete connect point");
    remove.addEventListener("click", () => actions.removeLink(link));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
}

// One line into a connect point: this consumer, and nothing else. What crosses, the scope and
// the transport belong to the point and are edited there. It opens only for a point with
// several consumers; clicking a point's only line opens the point.
function linePanel(design, link, consumer, actions) {
    const panel = document.createDocumentFragment();
    // The same opening as the other panels: the mark, the name, and what this line is.
    const head = tag("div", {class: "inspector__head"});
    head.append(contractSvg());
    const title = tag("h2", {class: "inspector__title"});
    title.append(linkTitleNode(link, consumer));
    head.append(title);
    panel.append(head);
    const carries = (link.members || []).length;
    panel.append(tag("p", {class: "inspector__help"},
                     `'${consumer}' consumes the connect point '${link.owner}' owns, so it `
                     + `acquires a replica of ${carries ? `the ${carries} member`
                                                          + `${carries === 1 ? "" : "s"} that `
                                                          + `cross${carries === 1 ? "es" : ""}`
                                                        : "whatever crosses"} it and can ask `
                     + `for ${carries === 1 ? "that" : "those"} and nothing else. What `
                     + `crosses belongs to the point, which every line into it shares.`));

    // The point this line belongs to, as a button that opens it.
    const button = tag("button", {type: "button", class: "button button--chip"});
    button.append(linkTitleNode(link));
    button.title = `Open the connect point ${linkTitle(link)}`;
    button.addEventListener("click", () => actions.openContract(link));
    panel.append(group("Owned by", button,
                       {help: [`Every consumer of '${link.owner || "this connect point"}' `
                               + `gets the same contract, so it is edited in one place: `
                               + `press this, or the contract icon on the canvas that every `
                               + `line leaves from.`]}));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       `Stop '${consumer}' consuming it`);
    remove.addEventListener("click", () => {
        link.consumers = (link.consumers || []).filter((name) => name !== consumer);
        actions.rebuild();
    });
    actionsRow.append(remove);
    panel.append(actionsRow);
    panel.append(note("Taking the last consumer off leaves the connect point where it is, "
                      + "drawn as a stub: a connect point exists before anything consumes "
                      + "it.", true));
    return panel;
}

// The project's scope vocabulary

// Everywhere a scope is still named. A scope in use cannot be removed, and the remove button
// says where it is used.
function usesOfScope(design, scope) {
    const found = [];
    for (const entity of design.entities || []) {
        if (entity.bundles && typeof entity.bundles === "object"
                && Object.prototype.hasOwnProperty.call(entity.bundles, scope)) {
            found.push(`${entity.name} serves it a bundle`);
        }
    }
    for (const link of design.links || []) {
        if (link.scope === scope) {
            found.push(`${link.owner}'s connect point is gated on it`);
        }
        for (const member of link.members || []) {
            if (member.scope === scope) {
                found.push(`${link.owner}.${member.name} is gated on it`);
            }
        }
        if (link.behind && link.behind[scope]) {
            found.push(`${link.owner} hands it to ${link.behind[scope]}`);
        }
    }
    return found;
}

function scopesPanel(design, actions) {
    // Renaming does not rebuild the panel (that would move the caret), so the drawn list goes
    // stale while a name is typed. Every handler reads the current list when it runs.
    const scopes = scopesOf(design).slice();
    const write = (next) => {
        design.scopes = next;
        actions.rebuild();
    };
    const rows = tag("div", {class: "scopes"});
    scopes.forEach((scope, index) => {
        const row = tag("div", {class: "scopes__row"});
        // A rename changes the vocabulary and nothing else: the gates, bundle keys and mapping
        // hook naming the old scope are left alone, and `synqt check` names each one on the
        // change sheet.
        const name = text(scope, (value) => {
            const wanted = value.trim();
            const current = scopesOf(design).slice();
            if (!wanted || current.includes(wanted)) {
                return;  // empty is a name half-typed. A duplicate is not a rename
            }
            current[index] = wanted;
            design.scopes = current;
            actions.changed();
        }, "scope");
        name.title = "Rename this scope. Anything gated on the old name keeps naming it, "
            + "and the change sheet says so.";
        row.append(name);
        // The order is the authority ranking under `scopes.hierarchical` and the values of the
        // mapping hook's generated enum, so moving a row renumbers the vocabulary.
        const up = tag("button", {type: "button", class: "icon-button",
                                  title: "Rank this scope lower"}, "^");
        up.disabled = index === 0;
        up.addEventListener("click", () => {
            const next = scopesOf(design).slice();
            next.splice(index - 1, 0, next.splice(index, 1)[0]);
            write(next);
        });
        const down = tag("button", {type: "button", class: "icon-button",
                                    title: "Rank this scope higher"}, "v");
        down.disabled = index === scopes.length - 1;
        down.addEventListener("click", () => {
            const next = scopesOf(design).slice();
            next.splice(index + 1, 0, next.splice(index, 1)[0]);
            write(next);
        });
        row.append(up, down);
        const used = usesOfScope(design, scope);
        const remove = remover(used.length
            ? `Still in use: ${used.join("; ")}`
            : "Remove this scope", () => {
            if (used.length) {
                return;  // the button's title says where it is used
            }
            write(scopesOf(design).filter((each) => each !== scopesOf(design)[index]));
        });
        remove.disabled = used.length > 0 || scopes.length <= 1;
        row.append(remove);
        rows.append(row);
    });
    return section("Scopes",
                   "What a session can be. The order is the ranking: a higher scope "
                   + "satisfies a lower one, and it is also what the mapping hook's "
                   + "generated enum counts from, so moving a row renumbers the vocabulary. "
                   + "The first is what a caller with no session holds. Renaming one renames "
                   + "it here only: whatever was gated on the old name still names it, and "
                   + "the change sheet refuses the plan until you say what it holds now.",
                   rows,
                   adder("Add scope", () => {
                       const current = scopesOf(design).slice();
                       let name = "scope";
                       for (let suffix = 2; current.includes(name); suffix += 1) {
                           name = `scope${suffix}`;
                       }
                       write(current.concat([name]));
                   }));
}

// Fill `host` with the panel for whatever is selected. `actions` is how the panel reports
// back: `changed` redraws, `rebuild` redraws and builds this panel again, `rename` carries a
// new name to the selection, and the two removers take the selection with them.
export function inspect(host, design, selected, actions) {
    // A change of selection closes every open row before the new panel is built.
    const now = selected ? `${selected.kind}\n${selected.name}` : "";
    if (now !== showing) {
        showing = now;
        forgetOpen();
    }
    host.replaceChildren();
    if (!selected) {
        // With nothing selected, the panel lists the gestures.
        const empty = tag("div", {class: "inspector__empty"});
        empty.append(tag("h2", {class: "block__title"}, "Nothing picked"));
        const how = tag("ul", {class: "inspector__how"});
        for (const step of [
            "Click an entity, a contract icon or a line to edit it.",
            "Drag from a handle on an entity's rim to another entity to draw a connect "
                + "point, owner first.",
            "Drop a line on empty canvas to make the entity it was reaching for.",
        ]) {
            how.append(tag("li", {}, step));
        }
        empty.append(how);
        host.append(empty);
        // And the project's scopes, which belong to no entity or link.
        host.append(scopesPanel(design, actions));
        return;
    }
    if (selected.kind === "entity") {
        const entity = (design.entities || []).find((one) => one.name === selected.name);
        if (entity) {
            host.append(entityPanel(design, entity, actions));
        }
        return;
    }
    const link = (design.links || []).find((one) => one.name === selected.name);
    if (!link) {
        return;
    }
    // A line selects one consumer; the icon selects the contract, the only place that changes
    // what crosses.
    if (selected.kind === "link" && selected.consumer) {
        host.append(linePanel(design, link, selected.consumer, actions));
        return;
    }
    host.append(contractPanel(design, link, actions));
}

export { TYPES, PROVIDER_FAMILIES };
