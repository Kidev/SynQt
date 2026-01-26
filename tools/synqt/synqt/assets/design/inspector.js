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

import { SCOPES, behindOf, entityType } from "./rules.js";
import { ROLE_HELP, accessorName, glyphSvg, memberMarkSvg, roleOf } from "./canvas.js";
import { linkTitle } from "./project.js";
import { baseType, declarations } from "./source.js";

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
function field(label, control, role) {
    const wrap = tag("label", {class: "field"});
    wrap.append(tag("span", {class: `field__label${role ? ` field__label--${role}` : ""}`},
                    label), control);
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
function group(label, controls, role) {
    const wrap = tag("div", {class: "field"});
    wrap.append(tag("span", {class: `field__label${role ? ` field__label--${role}` : ""}`},
                    label), controls);
    return wrap;
}

// A block of the panel with a heading and a rule above it. The panel is a column of settings
// with a paragraph under most of them, and without these the heading of the next block was one
// more line of prose in the same flow as the last block's explanation: eleven controls, no
// edges, nothing to scan for.
//
// The paragraphs come off the panel and go behind the `?` on the heading, which is the second
// half of that same job: with the rules in, the panel was a column of edges with more prose
// than controls between them, and a reader looking for the one setting they came for read a
// paragraph at each one to find it. Nothing is lost -- every word is still on the section it
// explains, one hover away -- and what is on screen is the settings.
function section(label, ...parts) {
    const box = tag("section", {class: "block"});
    const head = blockHead(box, label);
    for (const part of parts) {
        if (part) {
            box.append(part);
        }
    }
    explain(box, head);
    return box;
}

// The heading row of a block, and what the `?` hangs from. Its own function because two of
// these blocks are built a line at a time rather than out of finished parts, and a heading
// written a second way is a heading that explains itself a second way, or not at all.
function blockHead(box, label) {
    const head = tag("div", {class: "block__head"});
    head.append(tag("h2", {class: "block__title"}, label));
    box.append(head);
    return head;
}

// Move this section's paragraphs behind a `?` beside its heading.
//
// Unless they are all it has. A section whose whole content is its explanation -- "Nothing
// yet. Drag from a handle on this entity's rim" and the rest of the empty states -- is a
// heading over an empty box the moment the words are taken away, and an empty box explains
// itself to nobody. Decided on a copy, so the paragraphs are only ever detached once it is
// settled that they are going.
function explain(box, head) {
    const notes = Array.from(box.querySelectorAll("p.field__note"));
    if (!notes.length) {
        return;
    }
    const probe = box.cloneNode(true);
    probe.querySelector(".block__head").remove();
    for (const paragraph of Array.from(probe.querySelectorAll("p.field__note"))) {
        paragraph.remove();
    }
    if (!probe.textContent.trim() && !probe.querySelector("input, select, textarea, button")) {
        return;
    }
    const label = head.querySelector(".block__title").textContent;
    const ask = tag("button", {type: "button", class: "block__ask",
                               "aria-label": `What "${label}" means`}, "?");
    const tip = tag("span", {class: "block__tip", role: "tooltip"});
    for (const paragraph of notes) {
        paragraph.remove();
        paragraph.classList.remove("field__note--loose");
        tip.append(paragraph);
    }
    head.append(ask, tip);
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
// entity, a declaration): those are set in the code face, and a sentence about the entity is
// not. Everything wore the code face, which made a plain English switch read like something
// quoted out of a file and cost it a line of wrapping at this width.
function check(label, checked, onChange, {code} = {}) {
    const wrap = tag("label", {class: code ? "check check--code" : "check"});
    const box = tag("input", {type: "checkbox"});
    box.checked = checked;
    box.addEventListener("change", () => onChange(box.checked));
    wrap.append(box, tag("span", {class: "check__label"}, label));
    return wrap;
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
    panel.append(section("This entity",
        field("Name", text(entity.name, (value) => {
            renameEntity(design, entity, value);
            actions.rename("entity", value);
        })),
        // Stated, not offered. What an entity is was decided when it was dragged off the
        // palette, and everything drawn since means what it means because of that.
        group("Kind", tag("p", {class: "field__fixed"}, KIND_LABELS[role]))));

    const how = tag("div");
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
        how.append(group("Targets", targets));
        how.append(note("The browser bundle, a native desktop app from the same QML, or "
                        + "both. Either way it holds no secret and no mesh certificate."));
    } else if (role === "edge") {
        how.append(check("Runs the sign-in flow", Boolean(entity.identity), (on) => {
            entity.identity = on;
            actions.changed();
        }));
        how.append(note("The one entity the browser can reach. Everything else is "
                        + "behind it, on the mesh."));
        how.append(frontPanel(design, entity, actions));
    } else if (PROVIDER_FAMILIES.has(entityType(entity))) {
        how.append(field("Provider", text(entity.provider, (value) => {
            entity.provider = value;
            actions.changed();
        }, "sqlite")));
        how.append(note("The engine behind the type, swapped with this one value. "
                        + "Its credentials come from this entity's own environment and "
                        + "never from here."));
    }

    if (role !== "client") {
        const shared = typeof entity.shared === "boolean" ? entity.shared : true;
        // `rebuild` and not `changed`: the line under this switch is what the switch means,
        // and redrawing only the canvas left it saying the opposite of the box beside it.
        // A switch whose explanation does not move is a switch that looks like it did
        // nothing.
        how.append(check("One of it, for everybody", shared, (on) => {
            entity.shared = on;
            actions.rebuild();
        }));
        how.append(note(shared
            ? "One Source answers every caller, and each of them still arrives with a "
              + "Caller of their own. synqt.yaml says nothing, which is the default."
            : "Every caller gets a Source of their own, holding only what is theirs. "
              + "Written as `shared: false` on this entity."));
    }

    panel.append(section("How it runs", how));

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

    if (!owned.length && !owners.length) {
        return section("Wired to",
            note("Nothing yet. Drag from a handle on this entity's rim to another entity to "
                 + "draw a connect point out of it, or from that entity to this one to "
                 + "consume theirs.", true));
    }

    const box = tag("div");
    if (owned.length) {
        const point = owned[0];
        box.append(group("Consumers", consumers.length
            ? jumps(consumers, open)
            : tag("p", {class: "field__fixed"}, "nobody yet")));
        box.append(note(consumers.length
            ? `These acquire a replica of the connect point '${entity.name}' owns, and can `
              + "only ask it for what crosses. Nothing else on the mesh may reach it."
            : `'${entity.name}' owns a connect point that nothing consumes yet. Drag a line `
              + "from its rim to whatever should reach it."));
        const contract = tag("button", {type: "button", class: "button"},
                             `What crosses '${entity.name}'`);
        contract.addEventListener("click", () => actions.openContract(point));
        box.append(group("Contract", contract));
    }
    if (owners.length) {
        box.append(group("Owner", jumps([...new Set(owners)], open)));
        // A client says `Server` for the edge it reaches, which is the one accessor that is
        // not the owner's own name: it is a browser's word for whatever is in front of it.
        // Only ever one owner there, because the edge is the only thing a browser can reach.
        const reaches = roleOf(entity) === "client" ? "Server" : accessorName(owners[0]);
        box.append(note(owners.length === 1
            ? `'${entity.name}' consumes the connect point '${owners[0]}' owns, and reaches `
              + `it as \`${reaches}\` in its own QML.`
            : `'${entity.name}' consumes a connect point from each of these, and reaches each `
              + "one by that owner's own name."));
    }
    return section("Wired to", box);
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
    const head = blockHead(box, "What this entity declares");
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
    box.append(group("Add", adders));
    box.append(note(point || !models.length
        ? "A property, a signal and a function are written into this entity's own file, "
          + "which is where a connect point it owns finds them to put on a contract. A model "
          + "has no QML form, so it is written straight onto the connect point."
        : "Draw a connect point off this entity before adding a model: a model has no QML "
          + "form, so the connect point is the only place one can be written.", true));
    explain(box, head);
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
    }, "name")));

    // No size here, and no size on the parameters below: a declaration is kept as the line
    // in the file, and QML has no type with a limit in it. Where a size is a real thing is
    // on the connect point, beside the scope, and that is where the box for it is.
    if (member.kind === "prop") {
        row.append(field("Type", choice(TYPES, baseType(member.type) || "var", (value) => {
            member.type = value;
            actions.redeclare(entity, member, {...member}, member.name);
        })));
    }
    if (member.kind === "slot") {
        row.append(field("Answers", choice(["", ...TYPES], baseType(member.type), (value) => {
            member.type = value;
            actions.redeclare(entity, member, {...member}, member.name);
        }, "nothing")));
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
    summary.append(tag("span", {class: "member__kind"}, KIND_WORDS[member.kind]));
    // The name on its own, so an unfinished one is visibly unfinished rather than a line with
    // a gap in it. Everything after the name is the signature, quieter, because two members of
    // the same kind are told apart by their names.
    summary.append(member.name
        ? tag("span", {class: "member__name"}, member.name)
        : tag("span", {class: "member__name member__name--empty"}, "unnamed"));
    const rest = signatureRest(member);
    if (rest) {
        summary.append(tag("span", {class: "member__signature"}, rest));
    }
    summary.addEventListener("click", onToggle);
    head.append(summary);
    head.append(remover(removeTitle, onRemove));
    return head;
}

// Everything a declaration says after its own name: the type it holds, what it takes, what it
// answers. Written the way the file writes it, so it can be found in the file by searching for
// it.
function signatureRest(member) {
    if (member.kind === "prop") {
        return `: ${baseType(member.type) || "var"}`;
    }
    if (member.kind === "model") {
        return `(${roleList(member.roles)})`;
    }
    const params = (member.params || [])
        .map((param) => `${param.name}: ${baseType(param.type)}`).join(", ");
    return `(${params})${member.type ? `: ${baseType(member.type)}` : ""}`;
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
    }, "name")));
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
function memberText(member) {
    if (member.kind === "model") {
        return `model ${member.name}(${roleList(member.roles)})`;
    }
    if (member.kind === "prop") {
        return `prop ${member.type || "var"} ${member.name}`;
    }
    if (member.kind === "signal") {
        return `signal ${member.name}(${roleList(member.params)})`;
    }
    return `slot ${member.type ? `${member.type} ` : ""}${member.name}`
           + `(${roleList(member.params)})`;
}

function roleList(parts) {
    return (parts || []).map((part) => `${part.type} ${part.name}`).join(", ");
}

// The contract as a list to tick, out of what the owner entity already declares.
//
// This is the reading half of the same fact the entity panel writes: a member is declared on
// the entity, in the entity's own file, and it crosses a connect point because somebody
// ticked it here. Nothing is offered that the owner has not got, so a member cannot reach a
// contract without the file that implements it gaining the line first.
//
// One list, not one per consumer. The point has a single contract and every consumer gets the
// same one, which is what the drawing says too: every line leaves the one icon.
function ticksPanel(design, link, actions) {
    const box = tag("div", {class: "block members"});
    const head = blockHead(box, "What crosses this connect point");
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
        row.append(check(memberText(member), ticked.has(member.name), (on) => {
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
                })));
            }
            // The scope raises the bar for this member alone, so nothing about it crosses to
            // a caller the rest of the point reaches.
            extras.append(field("Scope", choice(["", ...SCOPES], carried.scope || "",
                                                (value) => {
                carried.scope = value;
                actions.changed();
            }, "the connect point's scope")));
            row.append(extras);
        } else {
            row.append(tickSummary(carried, sized, () => {
                opened.add(key);
                actions.rebuild();
            }));
        }
        list.append(row);
    }
    box.append(list);
    if (carriedOnly.length) {
        box.append(note(`${carriedOnly.length === 1 ? "One member is" : "These are"} `
                        + `written in '${link.owner}' in a form this panel reads on the `
                        + `entity rather than here: a model is written on the connect point `
                        + `itself, `
                        + `and a property set from a binding or a signal raised through `
                        + `Caller is code rather than a declaration. Untick one to take it `
                        + `off the contract.`, true));
    }

    const consumers = link.consumers || [];
    box.append(note(consumers.length
        ? `Ticked members are what '${link.owner}' says to '${consumers.join("', '")}'. `
          + "Nothing else ever crosses, and every consumer of this connect point gets the "
          + "same contract."
        : "Nothing consumes this connect point yet, so none of it reaches anywhere. Drag "
          + "from the contract icon to an entity.", true));
    explain(box, head);
    return box;
}

// What the connect point adds to one ticked member, in a line: the gate it is behind and the
// limit the owner-side boundary holds it to. Pressing it offers the two controls.
//
// A value that is set is coloured and a default is not, so the exceptions are what the eye
// finds down the column: most members are gated on the point's own scope and have no limit,
// and saying that eight times in a drop-down is eight ways to miss the ninth.
function tickSummary(carried, sized, onOpen) {
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
    line.addEventListener("click", onOpen);
    return line;
}

// Whether this edge hands its callers on, and where each scope currently goes.
//
// On the edge, with the rest of what an edge is: it sits beside "runs the sign-in flow"
// because it is the same kind of fact about the same entity, and an edge is where a reader
// goes to ask what the edge does. It was on the connect point, which is where the flag is
// written in synqt.yaml but not where anybody looks for it: the point's panel is about what
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
        // Nothing to split yet: an edge no browser consumes has no callers to hand on.
        return box;
    }
    const tiers = behindOf(link);
    const isFront = Boolean(link.behind);
    box.append(check("Hands callers to the entities behind it", isFront, (on) => {
        link.behind = on ? {...tiers} : undefined;
        if (!on) {
            delete link.behind;
        }
        actions.rebuild();
    }));
    if (!isFront) {
        box.append(note("The edge answers its own connect point. Turn this on to make it a "
                        + "front: it keeps the session and the sign-in, and hands each "
                        + "caller to the entity that serves people of their scope."));
        return box;
    }
    const wired = SCOPES.filter((scope) => tiers[scope]);
    box.append(note(wired.length
        ? `Drawn on the canvas: ${wired.map((scope) => `${scope} to ${tiers[scope]}`)
            .join(", ")}. Drag between a scope on the edge's back and an entity to change `
            + `one, either way round, or onto empty canvas to take it off.`
        : "Now drag between a scope on the edge's back and the entity that serves it, either "
          + "way round. Until one is wired the front hands nobody anywhere."));
    // What the switch has just done to everything already drawn, said where the switch is.
    // The rules paint the two halves of this that a drawing can decide on its own (a front
    // with nothing behind it, and a slot that answers a value, which a front cannot); the
    // third is a comparison of two contracts, and `synqt check` is what has that answer.
    const carries = (link.members || []).length;
    box.append(note(carries
        ? `'${entity.name}' no longer answers its own connect point. Every entity wired to a `
          + `scope has to carry the members this point offers callers of that scope, and `
          + `Review changes is what compares the two.`
        : `'${entity.name}' no longer answers its own connect point. What crosses it is `
          + `still declared here, and answered by whichever entity a caller's scope is `
          + `wired to.`));
    return box;
}

// The connect point itself, opened by clicking its icon on the canvas. Everything that is
// true of the point rather than of one line into it lives here: who owns it, who may consume
// it, the scope it is gated behind, how it is carried, and what crosses it.
function contractPanel(design, link, actions) {
    const panel = document.createDocumentFragment();
    // Named by its two ends, the same way the card on the canvas names it and the same way
    // the panel for one line into it does.
    panel.append(tag("h2", {class: "inspector__title"}, linkTitle(link)));

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
    }, "nobody yet"), "owner"));
    who.append(note("The owner is the name: a consumer reaches this point by writing "
                    + `${link.owner ? accessorName(link.owner) : "<Owner>"}, which is also `
                    + "the type it carries. An entity that already exports one is not "
                    + "offered here."));

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
    who.append(group("Consumers", consumers, "consumer"));
    who.append(note("This list is the authorization. An entity that is not on it is "
                    + "refused the replica, and nothing it does can talk its way on."));
    panel.append(section("The two ends", who));

    const reach = tag("div");
    // `rebuild`, because this is the default every member below inherits and the list of
    // members says what each one is gated on.
    reach.append(field("Scope", choice(["", ...SCOPES], link.scope || "", (value) => {
        link.scope = value;
        actions.rebuild();
    }, "any session, anonymous included")));
    reach.append(note("A browser below this scope never acquires the connect point at all, "
                      + "so its "
                      + "slots cannot be called and none of its state arrives. It is also "
                      + "the default for every member below: raise one of them on its own "
                      + "to keep an admin surface off a public page."));

    // `rebuild`, because the warning under this field appears and disappears with the value:
    // picking `local` used to change the transport and leave the page saying nothing about
    // what that costs until something else happened to redraw the panel.
    reach.append(field("Transport", choice(["", "local"], link.transport, (value) => {
        link.transport = value;
        actions.rebuild();
    }, "mutual TLS (the default)")));
    if (link.transport === "local") {
        reach.append(note("On a local socket the operating system identifies the connecting "
                          + "user, not the entity, so any process running as that user can "
                          + "present any entity name. Same host only."));
    }
    panel.append(section("Who may reach it", reach));

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
    panel.append(tag("h2", {class: "inspector__title"}, linkTitle(link, consumer)));
    panel.append(tag("p", {class: "inspector__help"},
                     `'${consumer}' consumes the connect point '${link.owner}' owns, `
                     + `so it acquires a replica of everything that point carries. `
                     + `What that is belongs to the point, not to this line.`));

    // The point this line is one of. The label says who decides and the button goes there,
    // which is the one thing this panel is for.
    const button = tag("button", {type: "button", class: "button"},
                       link.owner || "the connect point");
    button.addEventListener("click", () => actions.openContract(link));
    panel.append(group("Owned by", button));
    panel.append(note(`Every consumer of '${link.owner || "this connect point"}' `
                      + "gets the same contract, so it is edited in one place. The contract "
                      + "icon on the canvas, which every line leaves from, opens the same "
                      + "panel."));

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
// back: `changed` redraws, `rebuild` redraws and builds this panel again, `rename` carries a
// new name to the selection, and the two removers take the selection with them.
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
