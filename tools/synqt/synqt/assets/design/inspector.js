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
         glyphSvg, linkTitleNode, memberCode, memberMarkSvg,
         roleOf } from "./canvas.js";
import { linkTitle } from "./project.js";
import { baseType, declarations } from "./source.js";
import { contractBytes, memberSizeText, sizeText } from "./wire.js";

// The contract type vocabulary, from synqtc/types.py: QML's own built-in value types, and
// nothing invented beside them. A value crossing a connect point is read from QML on the
// owner side and handed to QML on the consumer side, so a type neither side can spell has
// nowhere to go. `float` is not one of them. The contract
// compiler refuses it, so offering it here would make this list a way to draw a project that will not build.
const TYPES = ["bool", "date", "double", "int", "list", "real", "string", "url", "var"];

// The four that take a bracketed size, and what one of that size is (synqtc.types
// SIZED_TYPES). Only the open-ended ones. A number says how wide it is by being a number,
// and what needs a limit is the value a caller can keep making bigger.
const SIZED = {string: "characters", url: "characters", list: "elements", var: "bytes"};

const KINDS = ["prop", "model", "signal", "slot"];

// What each kind is called where somebody is choosing one, and the QML word for it where
// there is one. A slot is written `function` in the file it lands in, so that is what the
// button that adds one says.
const KIND_WORDS = {prop: "property", model: "model", signal: "signal", slot: "function"};

// The three entity types that take a data provider (addentity.TYPES). An api and a
// jobs entity have no engine behind them, so neither is offered one.
const PROVIDER_FAMILIES = new Set(["relational", "cache", "document"]);

// What each type of entity is called in one line, for the panel to state rather than offer.
// An entity is whichever palette row it was dragged from and stays that: turning a database
// into a client in a drop-down would keep the name, the position and the connect points
// while changing what the thing fundamentally is, and everything drawn against it would
// silently mean something else. Delete it and drag the one you wanted.
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
// `help` is what that one control is, which is where an explanation belongs: on the setting
// it is about, and not gathered into a paragraph at the top of the section with the other
// ten. The mark that holds it goes beside the label, so the words being explained and the way
// to ask about them are the same three characters wide.
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
// Not a `<label>`, which is the whole reason this exists. A button and a checkbox are both
// labelable, so a `<label>` wrapped round several of them gives every one of them the same
// accessible name, taken from the whole group: four buttons reading property, model, signal
// and function all announced themselves as "Add property model signal function", which is
// what a screen reader says and what a test looking for the button named "property" fails
// to find.
function group(label, controls, {role, help} = {}) {
    const wrap = tag("div", {class: "field"});
    const name = tag("span", {class: `field__label${role ? ` field__label--${role}` : ""}`},
                     label);
    asked(name, label, help);
    wrap.append(name, controls);
    return wrap;
}

// A block of the panel with a heading and a rule above it. The panel is a column of settings
// with a paragraph under most of them, and without these the heading of the next block was one
// more line of prose in the same flow as the last block's explanation: eleven controls, no
// edges, nothing to scan for.
//
// `help` is what the whole block is, and it is short. Prose swept off the controls
// and piled behind the one mark on the heading would make a
// section explain eleven settings in one paragraph nobody can aim at. Each setting says
// what it is where it is, and this says what the group of them is for.
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

// The heading row of a block, and what its `?` hangs from. Its own function because two of
// these blocks are built a line at a time rather than out of finished parts, and a heading
// written a second way is a heading that explains itself a second way, or not at all.
function blockHead(box, label, help) {
    const head = tag("div", {class: "block__head"});
    head.append(tag("h2", {class: "block__title"}, label));
    asked(head, label, help);
    box.append(head);
    return head;
}

// The `?` beside a label, and the words it holds.
//
// Hidden until the pointer is on the section it belongs to, so a panel of settings reads as
// settings. Hovering the mark is what says what the setting is. Every word is still on the
// thing it explains, which is why each control gets its own mark rather than one paragraph
// at the top: the reader asks about the control they are looking at, and gets the
// answer to that question and no others.
//
// `host` is what the mark is appended to, and the tip is positioned against whichever
// ancestor of it is the width of the panel (`.field`, `.block__head`, `.helped`), because a
// paragraph in this column has always been that wide.
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

// A control that is not a labelled field (a switch, a row of them) with a mark of its own.
// The wrapper is what the explanation is measured against, and it is also what holds the mark
// out at the end of the line, away from the words of the switch itself.
function helped(node, label, help) {
    if (!help) {
        return node;
    }
    const wrap = tag("div", {class: "helped"});
    wrap.append(node);
    asked(wrap, label, help);
    return wrap;
}

// A line under whatever it explains. `loose` is for one that follows a heading or a button
// rather than a field. The tight form pulls itself up under the control it belongs to, and
// under a heading that reads as one line printed on top of another.
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

// A switch and what it says. `code` is for a label that is a name out of the project (an
// entity, a declaration). Those are set in the code face, and a sentence about the entity is
// not. Everything wore the code face, which made a plain English switch read like something
// quoted out of a file and cost it a line of wrapping at this width.
//
// `label` is a string, or the nodes to set as one: a member of a contract is a line of code
// and is painted the way the file it comes from is, which a string cannot carry.
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

// A type, and the size that goes with it where the type takes one. Two controls, because
// they are two decisions. `string` is the type and `[120]` is what the owner-side boundary
// refuses anything longer than. The box only appears for a type that can be given a limit,
// and leaving it empty is the ordinary case (no limit at all).
//
// `allowed` is the vocabulary to offer, so a slot's return can add the empty option for a
// slot that answers nothing without the size control losing track of which type is chosen.
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

function renameEntity(design, entity, wanted) {
    const before = entity.name;
    entity.name = wanted;
    for (const link of design.links || []) {
        if (link.owner === before) {
            // A connect point has no name of its own. The owner is the name, so all three
            // move together. Leaving `name` behind left the point keyed under an entity that
            // no longer existed, and every selection, every finding and every file in the
            // pane looked it up by that key.
            link.owner = wanted;
            link.name = wanted;
            link.id = wanted;
        }
        link.consumers = (link.consumers || [])
            .map((consumer) => (consumer === before ? wanted : consumer));
        // A front hands each scope to an entity by name, so a rename has to be carried in
        // here too or the routing points at somebody who is gone.
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
    // The name and the glyph the canvas draws this entity with, on one line, and under them
    // what this kind of entity is for. It reads as the panel's own opening statement rather
    // than as something quoted from elsewhere, which is what a rule down one side made of it.
    const head = tag("div", {class: "inspector__head"});
    head.append(glyphSvg(role));
    head.append(tag("h2", {class: "inspector__title"}, entity.name || "this entity"));
    panel.append(head);
    panel.append(tag("p", {class: "inspector__help"}, ROLE_HELP[role]));

    // What it is called, and what it is. Two facts about the thing itself, above everything
    // that is a decision about how it behaves.
    panel.append(section("This entity", "",
        field("Name", text(entity.name, (value) => {
            renameEntity(design, entity, value);
            actions.rename("entity", value);
        }), {help: "The name the rest of the project reaches this by: its folder on disk, "
                   + "the accessor other entities write in their QML, and the connect point "
                   + "it owns. Typing a new one here carries it into all of them at once."}),
        // Stated, not offered. What an entity is was decided when it was dragged off the
        // palette, and everything drawn since means what it means because of that.
        group("Kind", tag("p", {class: "field__fixed"}, KIND_LABELS[role]),
              {help: "What this entity is, chosen when it was dragged off the palette. It "
                     + "decides which Qt modules the entity links, where its files are "
                     + "written, and what it is allowed to reach. Drag a new entity out to "
                     + "have a different kind."})));

    const how = tag("div");
    if (entityType(entity) === "web_edge") {
        // Which scope is served which bundle. Read-only here. A value is either a client
        // entity or a directory, and `synqt check` refuses anything ambiguous, so this
        // shows what the topology says rather than offering a text box that could write a
        // mapping the build would then refuse.
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

    if (role !== "client") {
        const shared = typeof entity.shared === "boolean" ? entity.shared : true;
        // `rebuild` and not `changed`: what the switch means depends on where it is, so the
        // panel is built again around it. A switch whose surroundings do not move is a
        // switch that looks like it did nothing.
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

    panel.append(section("How it runs",
                         "How this entity behaves once it is running, and what it is built "
                         + "to link. Everything here is written onto this entity in "
                         + "synqt.yaml.", how));

    panel.append(wiredPanel(design, entity, actions));

    panel.append(declaresPanel(design, entity, actions));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete entity");
    remove.addEventListener("click", () => actions.removeEntity(entity));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
}

// A name on this panel that is another entity, as something to press. The panel is where a
// reader ends up after clicking one thing, and the next thing they want is usually at the
// other end of a line: reading "consumers: edge" and then having to find `edge` on the canvas
// is the panel naming a thing it will not take you to.
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

// Who is at the other end of every line this entity is on. The entities that consume the
// connect point it owns, and the owners of the points it consumes. The same two words
// synqt.yaml uses, so the panel and the file are one vocabulary.
//
// One connect point per owner, so "the point this entity owns" is one thing however the
// document is shaped. The consumers of it are the list. A point this entity consumes is
// named by its owner, which is why the owner is the name on the button.
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
        // Named the way everything else names a link, and the size of the buttons above it:
        // both are one press to somewhere else, and a wider one read as a different kind of
        // control. It names the thing it opens rather than the panel it opens ("What
        // crosses 'store'"), which would be a sentence where a name will do.
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
        // A client says `Server` for the edge it reaches, which is the one accessor that is
        // not the owner's own name. It is a browser's word for whatever is in front of it.
        // Only ever one owner there, because the edge is the only thing a browser can reach.
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
// This is the same gesture as writing the line into the file in the Files pane, and it is
// the pool every connect point this entity owns ticks its contract from. A member reaches a
// consumer because somebody ticked it there, and it is offered there because it was declared
// here. Reading is by the same parser the pane uses, so what is listed is exactly what the
// file says and never a second record of it kept alongside. Every control writes the line
// back, so the file stays the one record of it.
//
// Every part of a declaration that comes out of a fixed list is chosen from that list. Three
// buttons that each append a line with a placeholder name and a
// guessed type would leave the spelling of everything else to be typed into the file, a type
// list the contract compiler is the authority on, and a bracketed size whose grammar is not
// written anywhere near where somebody would be typing it.
//
// A model is here too. It has no QML declaration form, so it is not read out of the file. It
// is written straight onto the connect point this entity owns, which is the only place a
// model can live. It belongs on the entity all the same. It is one of the four things an
// entity can declare, and being the odd one out of the four is not a reason to make somebody
// look for it somewhere else.
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

// The connect point this entity owns, or null. An entity owns one, which is why the owner
// names it and why this can answer with a single thing.
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
// A declaration that is finished is a line of code, and a line of code is what it should look
// like, `property int highBid`, read at a glance against the file it is in. A permanent row
// of drop-downs and text boxes for every one of them would draw a member somebody added
// last week and a member half-typed this second identically, and eight of them would
// fill the panel with controls for eight decisions already made. So a row is the line it is,
// and opening it is what offers the controls.
//
// Kept here rather than in the document, because it is a state of this panel and not of the
// project. Nothing about which row is open is written to any file, and a reload starts closed.
const opened = new Set();

// What that set is keyed by. The line the declaration sits on, because that is what survives
// the thing most likely to happen while a row is open. Typing a new name into it. A key made
// of the name changes under the caret and closes the row mid-word.
function keyOf(entity, member) {
    return `${entity.name}\n${member.kind}\n${
        Number.isInteger(member.line) ? member.line : member.name}`;
}

// Whatever was open on one selection is closed by the time the next one is drawn: rows opened
// on an entity say nothing about the next entity, and a key that happens to collide would open
// a row nobody asked for.
function forgetOpen() {
    opened.clear();
    pending = "";
}

// The declaration a button has added, which is not in the panel yet. The file is rewritten
// and the panel is built again from what the file now says, so the row cannot be marked open
// until it exists. Named rather than keyed by line, because the line is what the rewrite
// decides.
//
// Pressing "property" is a request to declare one, and a row that arrives closed answers it
// with a line of code and nowhere to type the name.
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
// The kind is stated rather than offered. A property is not a function with a different word
// in front of it, and turning one into the other would rewrite a line whose body, bindings
// and call sites all belong to what it was. Delete it and add the one you wanted.
function declaredPanel(design, entity, member, actions) {
    // A declaration with no name yet is one that was added this second, so it opens itself:
    // the button that added it is a request to fill it in.
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
    // The captured member is what every control below writes through, so its name is kept in
    // step with the box. A rename is carried onto the contracts by comparing against the name
    // the line had, and reading that off a copy frozen at render time meant the second
    // keystroke of a rename was measured against the first one's result.
    row.append(field("Name", text(member.name, (value) => {
        const was = member.name;
        member.name = value;
        actions.redeclare(entity, member, {...member}, was);
    }, "name"),
                     {help: "What the declaration is called in the entity's own file, and "
                            + "what a consumer writes to reach it. Typing here rewrites the "
                            + "line in the file and carries the new name onto every contract "
                            + "already carrying it."}));

    // No size here, and no size on the parameters below. A declaration is kept as the line
    // in the file, and QML has no type with a limit in it. Where a size is a real thing is
    // on the connect point, beside the scope, and that is where the box for it is.
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

// The row a member is when it is not being edited: its kind, the line it declares, and the two
// buttons. The whole line is the control that opens it, because pointing at the thing is how
// anybody asks to change it.
function memberHead(member, open, onToggle, removeTitle, onRemove) {
    const head = tag("div", {class: "member__head"});
    const summary = tag("button", {type: "button", class: "member__summary",
                                   "aria-expanded": String(open)});
    summary.append(memberMarkSvg(member.kind));
    summary.append(tag("span", {class: "member__kind code__tok code__tok--kw"},
                       KIND_WORDS[member.kind]));
    // The name on its own, so an unfinished one is visibly unfinished rather than a line with
    // a gap in it. Everything after the name is the signature, quieter, because two members of
    // the same kind are told apart by their names.
    summary.append(member.name
        ? tag("span", {class: "member__name"}, member.name)
        : tag("span", {class: "member__name member__name--empty"}, "unnamed"));
    summary.append(signatureRest(member));
    summary.addEventListener("click", onToggle);
    head.append(summary);
    head.append(remover(removeTitle, onRemove));
    return head;
}

// Everything a declaration says after its own name. The type it holds, what it takes, what it
// answers. Written the way the file writes it, so it can be found in the file by searching for
// it, and painted the way the file paints it. A type is in the type colour here as much as in
// the pane, which is what makes a row of these read as the code it is rather than as a label.
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

// A model, which lives on the point rather than in the file. Its roles are what crosses,
// and only its roles. A row's other fields are dropped at the boundary.
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
// `sized` says whether a size can be set here, and it is not a matter of taste: a size is
// part of the contract and is kept in the connect point's `export:` block, so it survives
// only where these parts are the point's. A parameter read back out of the owner's QML has
// nowhere to keep one (`function add(text: string[200])` is a syntax error, not a limit), so
// offering the box there would be offering a setting that is gone on the next read.
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

// One member as the contract carries it. `prop`, `model`, `signal`, `slot`, which is the
// vocabulary of the `export:` block and of the canvas.
//
// One vocabulary down the whole list, whichever half of the pair a member came from. It used
// to say `function recordWinner(item: var)` for a member read out of the owner's QML and
// `slot recordWinner(var item)` for one that only exists on the point, so a single list mixed
// two words for the same thing and read as two lists that had been shuffled together. The
// entity's own panel is where the QML form belongs, and it says it there.
// The contract as a list to tick, out of what the owner entity already declares.
//
// This is the reading half of the same fact the entity panel writes. A member is declared on
// the entity, in the entity's own file, and it crosses a connect point because somebody
// ticked it here. Nothing is offered that the owner has not got, so a member cannot reach a
// contract without the file that implements it gaining the line first.
//
// One list, not one per consumer. The point has a single contract and every consumer gets the
// same one, which is what the drawing says too: every line leaves the one icon.
function ticksPanel(design, link, actions) {
    const box = tag("div", {class: "block members"});
    const head = blockHead(box, "What crosses it",
              [`Everything ticked here is what '${link.owner || "the owner"}' says to `
               + `whoever consumes this point, and it is what the generated replica carries. `
               + `Nothing else ever crosses.`,
               "The list is what the owner entity declares, so a member reaches a consumer "
               + "because somebody ticked it here, and the file that implements it already "
               + "has the line."]);
    // And how much of it. The contract says what crosses. The one thing it did not say was
    // how big that is, which is what decides whether a property is pushed on a keystroke or
    // on a timer. The number is a ceiling worked out from the sizes in the contract itself
    // (a `string[60]` is four bytes of length and at most sixty UTF-16 characters), never a
    // measurement of anything running, and it says so.
    head.append(wireSize(link));
    const owner = (design.entities || []).find((one) => one.name === link.owner);
    if (!owner) {
        box.append(note("No owner yet, so there is nothing to carry.", true));
        return box;
    }

    // Everything the owner declares, and everything the point already carries. The two are
    // nearly the same list and neither one alone is it. A model has no QML declaration form
    // and would never be offered, and a member an owner writes in a form this reader does not
    // follow (a property set from a binding, a signal raised through `Caller`) is on the
    // contract and was missing from the panel, with no way to see it or take it off.
    // `synqt check` reads all of those and holds the owner to them. Until this listed them
    // the panel showed a contract with members it had quietly left out.
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
        // The two things about a ticked member that are not in the owner's file, so the two
        // things this list sets. Offered only once the member is on the contract: either one
        // on something that does not cross is a setting with no effect.
        //
        // Under the member rather than beside it, each behind a label of its own. Beside it
        // they were two unlabelled controls competing with the member's own name for one
        // panel's width, and the drop-down, being a full-width control in a flex row, won:
        // the row showed a scope selector and no sign of which member it was on.
        const carried = (link.members || []).find((one) => one.name === member.name);
        if (!carried) {
            list.append(row);
            continue;
        }
        // What the point adds to this member: a gate, and where the type takes one, a limit.
        // Both are set on most members and never on the rest, so the row says what they are
        // in a line and offers the controls when that line is pressed. Left open, every
        // ticked member cost two labelled drop-downs, and a contract of eight was a column of
        // sixteen controls with the contract lost between them.
        const key = `${link.owner}\ntick\n${member.name}`;
        const sized = carried.kind === "prop" && SIZED[baseType(carried.type)];
        if (opened.has(key)) {
            const extras = tag("div", {class: "tick__extras"});
            // The size is what the owner-side boundary refuses anything longer than, and it
            // is here because it is part of the contract: QML has no type with a limit in
            // it, so the declaration in the file cannot hold one.
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
            // The scope raises the bar for this member alone, so nothing about it crosses to
            // a caller the rest of the point reaches.
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

// What the connect point adds to one ticked member, in a line. The gate it is behind and the
// limit the owner-side boundary holds it to. Pressing it offers the two controls.
//
// A value that is set is coloured and a default is not, so the exceptions are what the eye
// finds down the column. Most members are gated on the point's own scope and have no limit,
// and saying that eight times in a drop-down is eight ways to miss the ninth.
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

// What the whole contract costs, beside the heading that says what it carries.
//
// One crossing of each member, with a model counted as one row. Not a rate, because how often
// anything crosses is the application's business, but the size of the wire, which is what
// somebody sizing one is asking. A contract holding one member with no limit on it has no
// ceiling at all, and the chip says that rather than printing the bounded part as if it were
// the answer.
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
// On the edge, with the rest of what an edge is. It sits beside "runs the sign-in flow"
// because it is the same kind of fact about the same entity, and an edge is where a reader
// goes to ask what the edge does. It was on the connect point, which is where the flag is
// written in synqt.yaml but not where anybody looks for it. The point's panel is about what
// crosses the link, and an entity's whole job was one panel further away than the smallest
// thing about it.
//
// Only the switch. A front is drawn as a wedge with a seat per scope along its back, and a
// scope is handed to an entity by dragging between the two. The routing is read off the
// picture rather than out of a list of drop-downs, which is the whole reason to draw a
// system instead of writing it.
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
    // What the switch means, on the switch. Turned on, this edge stops answering its own
    // connect point and every caller is served by whichever entity their scope is wired to.
    // The rules paint the two halves of that a drawing can decide on its own (a front with
    // nothing behind it, and a slot that answers a value, which a front cannot). The third is
    // a comparison of two contracts, and `synqt check` is what has that answer.
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
    // Where each scope currently goes, which is a fact about this drawing rather than an
    // explanation of it, so it stays on the panel. It is the one part of the routing that is
    // read rather than seen, the seats on the canvas being small.
    box.append(note(wired.length
        ? `Drawn on the canvas: ${wired.map((scope) => `${scope} to '${tiers[scope]}'`)
            .join(", ")}.`
        : "No scope is wired yet, so this front hands nobody anywhere. Drag between a scope "
          + "on its back and the entity that serves it, either way round."));
    return box;
}

// What having selected this connect point means, said in the names of the entities at its
// two ends rather than in the words owner and consumer. It is the opening line of the panel,
// and the panel opens because somebody clicked a thing on the canvas. What they want first is
// what that thing does, with the names they gave it.
function contractSays(design, link) {
    const consumers = link.consumers || [];
    const carries = (link.members || []).length;
    const how = link.transport === "local" ? "a local socket on this host"
                                           : "the mesh, over mutual TLS";
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

// The connect point itself, opened by clicking its icon on the canvas. Everything that is
// true of the point rather than of one line into it lives here. Who owns it, who may consume
// it, the scope it is gated behind, how it is carried, and what crosses it.
function contractPanel(design, link, actions) {
    const panel = document.createDocumentFragment();
    // Opened the way an entity's panel opens. The mark the thing was clicked on, its name,
    // and one line saying what it is in the names of the two entities it runs
    // between. The two names and nothing else would name the selection and
    // then leave a reader to work out what having selected it means.
    const head = tag("div", {class: "inspector__head"});
    head.append(contractSvg());
    const title = tag("h2", {class: "inspector__title"});
    title.append(linkTitleNode(link));
    head.append(title);
    panel.append(head);
    panel.append(tag("p", {class: "inspector__help"}, contractSays(design, link)));

    // Nothing to name. An entity has one connect point, so the owner names it: consumers
    // reach it as the owner capitalised, and the contract carries that same name.
    const names = (design.entities || []).map((entity) => entity.name);
    const taken = new Set((design.links || [])
        .filter((one) => one !== link)
        .map((one) => one.owner));
    const who = tag("div");
    // The two labels take the two role colours the canvas and the tip use for the same two
    // words, so a reader who has hovered one line already knows which half of this panel is
    // which without reading either heading.
    who.append(field("Owner", choice(["", ...names.filter((name) => !taken.has(name))],
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
                             "The list is the authorization: it is checked against the "
                             + "verified name on the caller's own certificate, so an entity "
                             + "reaches this point because it is ticked here and for no "
                             + "other reason."]}));
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

    // `rebuild`, because what is said about the transport changes with the value. Picking
    // `local` must not change the transport and leave the page saying nothing about what
    // that costs until something else happens to redraw the panel.
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

// One line into a connect point. This consumer, and nothing else.
//
// Almost empty. A line is an entity name on a point consumer list (not something
// with its own settings), and everything a reader might come here to change (what crosses,
// the scope, the transport) belongs to the point and is edited on the point. Offering those
// here would be offering to edit one shared contract from N places and letting somebody
// believe they had changed it for this consumer alone.
//
// It only ever opens for a point with several consumers. One line and one point are the same
// selection to anybody who drew them, so clicking the only line opens the point itself.
function linePanel(design, link, consumer, actions) {
    const panel = document.createDocumentFragment();
    // The same opening an entity and a connect point get: the mark, the name, and a line
    // saying what this one is. A line is the narrowest selection on the canvas, so it is the
    // one that most needs saying what it is a selection of.
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

    // The point this line is one of. The label says who decides and the button goes there,
    // which is the one thing this panel is for.
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

// Fill `host` with the panel for whatever is selected. `actions` is how the panel reports
// back. `changed` redraws, `rebuild` redraws and builds this panel again, `rename` carries a
// new name to the selection, and the two removers take the selection with them.

// The project's scope vocabulary

// Everywhere a scope is still named, for a scope somebody is about to remove. Removing one
// out from under a gate leaves a project `synqt check` refuses, and the panel says where
// rather than refusing with nothing to go on.
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
    // The list as it is drawn, and the list as it is *now*, which are two different things
    // while a name is being typed. Renaming does not rebuild the panel, because rebuilding
    // it under the caret would take the caret with it. So every handler below reads the
    // current list at the moment it runs rather than the one this render closed over. That
    // is not a detail. Closing over the drawn list meant pressing Add after typing a new
    // name wrote the old name back, silently undoing the rename.
    const scopes = scopesOf(design).slice();
    const write = (next) => {
        design.scopes = next;
        actions.rebuild();
    };
    const rows = tag("div", {class: "scopes"});
    scopes.forEach((scope, index) => {
        const row = tag("div", {class: "scopes__row"});
        // Renaming is typed in place and changes the vocabulary, nothing else. It does not
        // go looking for the gates, the bundle keys or the mapping hook that named the old
        // scope. Telling a rename from a removal and an addition means guessing from two
        // snapshots of a list, and a guess that lands wrong edits a file nobody pointed at.
        // So a rename leaves its uses where they are, and `synqt check` names each one on
        // the change sheet before there is anything to apply.
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
        // The order is the authority ranking under `scopes.hierarchical`, and since the
        // mapping hook answers with a generated enum it is that enum's member values too, so
        // moving a row renumbers the vocabulary. That is why the arrows are here and not
        // an add and a remove.
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
                return;  // named on the button, so the answer is on the thing pressed
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

export function inspect(host, design, selected, actions) {
    // A row opened on one selection says nothing about the next one, so a change of selection
    // closes everything before the new panel is built.
    const now = selected ? `${selected.kind}\n${selected.name}` : "";
    if (now !== showing) {
        showing = now;
        forgetOpen();
    }
    host.replaceChildren();
    if (!selected) {
        // The panel with nothing in it is the one place a first reader has room to be told
        // what the two gestures are. One paragraph of prose said the same thing and read as
        // something to skip. Three lines, each a thing to do, is a list somebody can act on.
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
        // And the one setting that belongs to the project rather than to anything on the
        // canvas. It lives here because there is nowhere else it could. A scope is not an
        // entity and not a link, and every gate in the panel above chooses from this list.
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
    // A line into the point, or the point itself. Clicking one line says "this consumer";
    // clicking the icon every line leaves from says "this contract", and only the second one
    // is allowed to change what crosses.
    if (selected.kind === "link" && selected.consumer) {
        host.append(linePanel(design, link, selected.consumer, actions));
        return;
    }
    host.append(contractPanel(design, link, actions));
}

export { TYPES, PROVIDER_FAMILIES };
