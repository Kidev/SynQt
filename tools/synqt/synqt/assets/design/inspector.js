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
import { ROLE_HELP, accessorName, glyphSvg, roleOf } from "./canvas.js";
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
function field(label, control) {
    const wrap = tag("label", {class: "field"});
    wrap.append(tag("span", {class: "field__label"}, label), control);
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
function group(label, controls) {
    const wrap = tag("div", {class: "field"});
    wrap.append(tag("span", {class: "field__label"}, label), controls);
    return wrap;
}

// A line under whatever it explains. `loose` is for one that follows a heading or a button
// rather than a field: the tight form pulls itself up under the control it belongs to, and
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

function check(label, checked, onChange) {
    const wrap = tag("label", {class: "check"});
    const box = tag("input", {type: "checkbox"});
    box.checked = checked;
    box.addEventListener("change", () => onChange(box.checked));
    wrap.append(box, document.createTextNode(label));
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

// The entity panel

function renameEntity(design, entity, wanted) {
    const before = entity.name;
    entity.name = wanted;
    for (const link of design.links || []) {
        if (link.owner === before) {
            link.owner = wanted;
        }
        link.consumers = (link.consumers || [])
            .map((consumer) => (consumer === before ? wanted : consumer));
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

    panel.append(field("Name", text(entity.name, (value) => {
        renameEntity(design, entity, value);
        actions.rename("entity", value);
    })));

    // Stated, not offered. What an entity is was decided when it was dragged off the
    // palette, and everything drawn since means what it means because of that.
    panel.append(group("Kind", tag("p", {class: "field__fixed"}, KIND_LABELS[role])));

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
            }));
        }
        panel.append(group("Targets", targets));
        panel.append(note("The browser bundle, a native desktop app from the same QML, or "
                          + "both. Either way it holds no secret and no mesh certificate."));
    } else if (role === "edge") {
        panel.append(check("Runs the sign-in flow", Boolean(entity.identity), (on) => {
            entity.identity = on;
            actions.changed();
        }));
        panel.append(note("The one entity the browser can reach. Everything else is "
                          + "behind it, on the mesh."));
    } else if (PROVIDER_FAMILIES.has(entityType(entity))) {
        panel.append(field("Provider", text(entity.provider, (value) => {
            entity.provider = value;
            actions.changed();
        }, "sqlite")));
        panel.append(note("The engine behind the type, swapped with this one value. "
                          + "Its credentials come from this entity's own environment and "
                          + "never from here."));
    }

    if (role !== "client") {
        const shared = typeof entity.shared === "boolean" ? entity.shared : true;
        panel.append(check("One of it, for everybody", shared, (on) => {
            entity.shared = on;
            actions.changed();
        }));
        panel.append(note(shared
            ? "One Source answers every caller, and each of them still arrives with a "
              + "Caller of their own."
            : "Every caller gets a Source of their own, holding only what is theirs."));
    }

    panel.append(declaresPanel(design, entity, actions));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete entity");
    remove.addEventListener("click", () => actions.removeEntity(entity));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
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
    const box = tag("div", {class: "members"});
    box.append(tag("h2", {class: "members__title"}, "What this entity declares"));
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
          + "has no QML form, so it is written straight onto the point."
        : "Draw a connect point off this entity before adding a model: a model has no QML "
          + "form, so the point is the only place one can be written.", true));
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
    actions.rebuild();
}

// One declaration the file holds, as controls over the line it is written on.
//
// The kind is stated rather than offered: a property is not a function with a different word
// in front of it, and turning one into the other would rewrite a line whose body, bindings
// and call sites all belong to what it was. Delete it and add the one you wanted.
function declaredPanel(design, entity, member, actions) {
    const box = tag("div", {class: "member"});
    const row = tag("div", {class: "member__row"});

    row.append(tag("span", {class: "member__kind"}, KIND_WORDS[member.kind]));
    row.append(text(member.name, (value) => {
        actions.redeclare(entity, member, {...member, name: value}, member.name);
    }, "name"));

    // No size here, and no size on the parameters below: a declaration is kept as the line
    // in the file, and QML has no type with a limit in it. Where a size is a real thing is
    // on the connect point, beside the scope, and that is where the box for it is.
    if (member.kind === "prop") {
        row.append(choice(TYPES, baseType(member.type) || "var", (value) => {
            actions.redeclare(entity, member, {...member, type: value}, member.name);
        }));
    }
    if (member.kind === "slot") {
        row.append(choice(["", ...TYPES], baseType(member.type), (value) => {
            actions.redeclare(entity, member, {...member, type: value}, member.name);
        }, "returns nothing"));
    }
    row.append(remover(`Remove ${member.name || "this declaration"}`,
                       () => actions.undeclare(entity, member)));
    box.append(row);

    if (member.kind === "signal" || member.kind === "slot") {
        const write = () => actions.redeclare(entity, member, member, member.name);
        box.append(partsPanel(member, "params", "Parameters",
                              {changed: write, rebuild: write}, false));
    }
    return box;
}

// A model, which lives on the point rather than in the file. Its roles are what crosses,
// and only its roles: a row's other fields are dropped at the boundary.
function modelPanel(point, model, actions) {
    const box = tag("div", {class: "member"});
    const row = tag("div", {class: "member__row"});
    row.append(tag("span", {class: "member__kind"}, "model"));
    row.append(text(model.name, (value) => {
        model.name = value;
        actions.changed();
    }, "name"));
    row.append(remover(`Remove ${model.name || "this model"}`, () => {
        point.members = (point.members || []).filter((one) => one !== model);
        actions.rebuild();
    }));
    box.append(row);
    box.append(partsPanel(model, "roles", "Roles", actions, true));
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

// One declaration, as the QML it is. The list shows the file's own words rather than a
// prettier restatement of them, so what is listed can be found by searching the file.
function declaredText(member) {
    if (member.kind === "prop") {
        return `property ${baseType(member.type) || "var"} ${member.name}`;
    }
    const params = (member.params || [])
        .map((param) => `${param.name}: ${baseType(param.type)}`).join(", ");
    if (member.kind === "signal") {
        return `signal ${member.name}(${params})`;
    }
    return `function ${member.name}(${params})`
           + `${member.type ? `: ${baseType(member.type)}` : ""}`;
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
    const box = tag("div", {class: "members"});
    box.append(tag("h2", {class: "members__title"}, "What crosses this connect point"));
    const owner = (design.entities || []).find((one) => one.name === link.owner);
    if (!owner) {
        box.append(note("No owner yet, so there is nothing to carry.", true));
        return box;
    }

    const offered = declarations(owner.qml || "");
    const ticked = new Set((link.members || []).map((member) => member.name));
    if (!offered.length) {
        box.append(note(`'${link.owner}' declares nothing yet, so this contract is not `
                        + "finished. Add a property, a signal or a function on the entity "
                        + "and it will be here to tick.", true));
    }

    const list = tag("div", {class: "ticks"});
    for (const member of offered) {
        const row = tag("div", {class: "tick"});
        row.append(check(declaredText(member), ticked.has(member.name), (on) => {
            actions.tick(link, member, on);
        }));
        // The two things about a ticked member that are not in the owner's file, so the two
        // things this list sets. Offered only once the member is on the contract: either one
        // on something that does not cross is a setting with no effect.
        //
        // The scope raises the bar for this member alone, so nothing about it crosses to a
        // caller the rest of the point reaches. The size is what the owner-side boundary
        // refuses anything longer than, and it is here because it is part of the contract:
        // QML has no type with a limit in it, so the declaration cannot hold one.
        const carried = (link.members || []).find((one) => one.name === member.name);
        if (carried && carried.kind === "prop" && SIZED[baseType(carried.type)]) {
            row.append(typeAndSize(carried.type, (value) => {
                carried.type = value;
                actions.changed();
            }));
        }
        if (carried) {
            row.append(choice(["", ...SCOPES], carried.scope || "", (value) => {
                carried.scope = value;
                actions.changed();
            }, "the point's scope"));
        }
        list.append(row);
    }
    box.append(list);

    // The models this point carries, stated rather than edited. A model has no QML
    // declaration form, so it is written onto the point itself, but it is one of the four
    // things an entity declares and it is declared with the other three, on the entity.
    const models = (link.members || []).filter((one) => one.kind === "model");
    for (const model of models) {
        list.append(tag("code", {class: "declares__line"},
                        `model ${model.name}(${(model.roles || [])
                            .map((role) => `${role.type} ${role.name}`).join(", ")})`));
    }
    if (models.length) {
        box.append(note(`Edited on '${link.owner}', with everything else that entity `
                        + "declares.", true));
    }

    const consumers = link.consumers || [];
    box.append(note(consumers.length
        ? `Ticked members are what '${link.owner}' says to '${consumers.join("', '")}'. `
          + "Nothing else ever crosses, and every consumer of this point gets the same "
          + "contract."
        : "Nothing consumes this point yet, so none of it reaches anywhere. Drag from the "
          + "contract icon to an entity.", true));
    return box;
}

// Whether this point is a front, and where each scope currently goes.
//
// The switch is here because becoming a front is a decision about the point; the wiring is
// not, and there is no control for it here on purpose. A front is drawn as a wedge with a
// seat per scope on its flat side, and a scope is handed to an entity by dragging from its
// seat onto that entity. That way the routing is read off the picture instead of out of a
// list of drop-downs, which is the whole reason to draw a system rather than write it.
function frontPanel(design, link, actions) {
    const box = document.createDocumentFragment();
    const entities = design.entities || [];
    const owner = entities.find((entity) => entity.name === link.owner);
    const clients = new Set(entities.filter((entity) => entityType(entity) === "client")
                                    .map((entity) => entity.name));
    if (!owner || entityType(owner) !== "web_edge"
        || !(link.consumers || []).some((consumer) => clients.has(consumer))) {
        return box;   // only a browser-facing web edge has callers to split
    }
    const tiers = behindOf(link);
    const isFront = Boolean(link.behind);
    box.append(check("Hand callers to entities behind it", isFront, (on) => {
        link.behind = on ? {...tiers} : undefined;
        if (!on) {
            delete link.behind;
        }
        actions.rebuild();
    }));
    if (!isFront) {
        box.append(note("The edge answers this point itself. Turn this on to make it a "
                        + "front: it keeps the session and the sign-in, and hands each "
                        + "caller to the entity that serves people of their scope.", true));
        return box;
    }
    const wired = SCOPES.filter((scope) => tiers[scope]);
    box.append(note(wired.length
        ? `Drawn on the canvas: ${wired.map((scope) => `${scope} to ${tiers[scope]}`)
            .join(", ")}. Drag from a seat on the flat side to change one, or onto empty `
            + `canvas to take it off.`
        : "Now drag from a seat on the wedge's flat side onto the entity that serves that "
          + "scope. Until one is wired the front hands nobody anywhere.", true));
    return box;
}

// The connect point itself, opened by clicking its icon on the canvas. Everything that is
// true of the point rather than of one line into it lives here: who owns it, who may consume
// it, the scope it is gated behind, how it is carried, and what crosses it.
function contractPanel(design, link, actions) {
    const panel = document.createDocumentFragment();
    panel.append(tag("h2", {class: "inspector__title"},
                     link.owner ? `${link.owner}'s connect point` : "this connect point"));

    // Nothing to name. An entity has one connect point, so the owner names it: consumers
    // reach it as the owner capitalised, and the contract carries that same name.
    const names = (design.entities || []).map((entity) => entity.name);
    const taken = new Set((design.links || [])
        .filter((one) => one !== link)
        .map((one) => one.owner));
    panel.append(field("Owner", choice(["", ...names.filter((name) => !taken.has(name))],
                                       link.owner, (value) => {
        link.owner = value;
        link.id = value;
        link.name = value;
        link.consumers = (link.consumers || []).filter((consumer) => consumer !== value);
        actions.rebuild();
    }, "nobody yet")));
    panel.append(note("The owner is the name: consumers reach this as "
                      + `${link.owner ? accessorName(link.owner) : "<Owner>"}, and it `
                      + "carries the "
                      + `${link.owner ? accessorName(link.owner) : "<Owner>"} type. `
                      + "An entity that already exports one is not offered here."));

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
            actions.changed();
        }));
    }
    panel.append(group("Consumers", consumers));
    panel.append(note("This list is the authorization. An entity that is not on it is "
                      + "refused the replica, and nothing it does can talk its way on."));

    panel.append(field("Scope", choice(["", ...SCOPES], link.scope || "", (value) => {
        link.scope = value;
        actions.changed();
    }, "any session, anonymous included")));
    panel.append(note("A browser below this scope never acquires the point at all, so its "
                      + "slots cannot be called and none of its state arrives. It is also "
                      + "the default for every member below: raise one of them on its own "
                      + "to keep an admin surface off a public page."));

    panel.append(frontPanel(design, link, actions));

    panel.append(field("Transport", choice(["", "local"], link.transport, (value) => {
        link.transport = value;
        actions.changed();
    }, "mutual TLS (the default)")));
    if (link.transport === "local") {
        panel.append(note("On a local socket the operating system identifies the connecting "
                          + "user, not the entity, so any process running as that user can "
                          + "present any entity name. Same host only."));
    }

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
    panel.append(tag("h2", {class: "inspector__title"},
                     `${link.owner} to ${consumer}`));
    panel.append(tag("p", {class: "inspector__help"},
                     `'${consumer}' consumes ${link.owner ? accessorName(link.owner) : ""}, `
                     + `so it acquires a replica of everything that point carries. What that `
                     + `is belongs to the point, not to this line.`));

    // The point this line is one of, named as a consumer names it. The label says who
    // decides and the button goes there, which is the one thing this panel is for.
    const button = tag("button", {type: "button", class: "button"},
                       accessorName(link.owner) || "the connect point");
    button.addEventListener("click", () => actions.openContract(link));
    panel.append(group("Owned by", button));
    panel.append(note(`Every consumer of ${accessorName(link.owner) || "this point"} gets the `
                      + "same contract, so it is edited in one place. The contract icon on "
                      + "the canvas, which every line out of the point leaves from, opens the "
                      + "same panel."));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       `Stop '${consumer}' consuming it`);
    remove.addEventListener("click", () => {
        link.consumers = (link.consumers || []).filter((name) => name !== consumer);
        actions.rebuild();
    });
    actionsRow.append(remove);
    panel.append(actionsRow);
    panel.append(note("Taking the last consumer off leaves the point where it is, drawn as "
                      + "a stub: a connect point exists before anything consumes it.", true));
    return panel;
}

// Fill `host` with the panel for whatever is selected. `actions` is how the panel reports
// back: `changed` redraws, `rebuild` redraws and builds this panel again, `rename` carries a
// new name to the selection, and the two removers take the selection with them.
export function inspect(host, design, selected, actions) {
    host.replaceChildren();
    if (!selected) {
        host.append(tag("p", {class: "inspector__empty"},
                        "Pick an entity, a contract or a line to edit it. Drag from the "
                        + "handle on an entity's edge to another entity to draw a connect "
                        + "point between them, from the owner to the consumer."));
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
