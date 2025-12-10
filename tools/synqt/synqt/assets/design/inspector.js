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
import { ROLE_HELP, glyphSvg, roleOf } from "./canvas.js";
import { declarations } from "./source.js";

// The .syn type vocabulary, from synqtc/types.py. `var` is in it because a model role may
// carry anything, and the roles are where that comes up.
const TYPES = ["int", "string", "bool", "real", "float", "double", "var"];

const KINDS = ["prop", "model", "signal", "slot"];

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

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}


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

function field(label, control) {
    const wrap = tag("label", {class: "field"});
    wrap.append(tag("span", {class: "field__label"}, label), control);
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
    panel.append(field("Kind", tag("p", {class: "field__fixed"}, KIND_LABELS[role])));

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
        panel.append(field("Targets", targets));
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

    panel.append(declaresPanel(entity, actions));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete entity");
    remove.addEventListener("click", () => actions.removeEntity(entity));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
}

// What the entity's own file declares, and the way to add one without typing it.
//
// This is the same gesture as writing the line into the file in the Files pane, and it is
// the pool every connect point this entity owns ticks its contract from: a member reaches a
// consumer because somebody ticked it there, and it is offered there because it was declared
// here. Reading is by the same parser the pane uses, so what is listed is exactly what the
// file says and never a second record of it kept alongside.
function declaresPanel(entity, actions) {
    const box = tag("div", {class: "members"});
    box.append(tag("h2", {class: "members__title"}, "What this entity declares"));
    const found = declarations(entity.qml || "");
    if (!found.length) {
        box.append(note("Nothing yet. A property is state, a signal is something that "
                        + "happened, and a function is something callers can ask for.", true));
    }
    const list = tag("div", {class: "declares"});
    for (const member of found) {
        list.append(tag("code", {class: "declares__line"}, declaredText(member)));
    }
    box.append(list);
    for (const [label, member] of [
        ["Add a property", {kind: "prop", name: "", type: "int"}],
        ["Add a signal", {kind: "signal", name: "", params: []}],
        ["Add a function", {kind: "slot", name: "", type: "", params: []}],
    ]) {
        box.append(adder(label, () => actions.declare(entity, member)));
    }
    box.append(note("Each one is written into this entity's own file, which is where a "
                    + "connect point it owns finds it to put on a contract.", true));
    return box;
}

// One declaration, as the QML it is. The panel shows the file's own words rather than a
// prettier restatement of them, so what is listed can be found by searching the file.
function declaredText(member) {
    if (member.kind === "prop") {
        return `property ${member.type || "var"} ${member.name}`;
    }
    const params = (member.params || [])
        .map((param) => `${param.name}: ${param.type}`).join(", ");
    if (member.kind === "signal") {
        return `signal ${member.name}(${params})`;
    }
    return `function ${member.name}(${params})${member.type ? `: ${member.type}` : ""}`;
}

// The connect point panel

function partsPanel(member, key, label, actions) {
    const box = tag("div", {class: "member__parts"});
    box.append(tag("div", {class: "member__parts-title"}, label));
    const parts = member[key] || [];
    parts.forEach((part, index) => {
        const row = tag("div", {class: "member__part"});
        row.append(choice(TYPES, part.type || "string", (value) => {
            part.type = value;
            actions.changed();
        }));
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

// What a member carries depends on what it is, so changing its kind drops what the new kind
// has no place for rather than keeping a hidden list that would come back later.
function settle(member) {
    if (member.kind === "prop") {
        member.type = member.type || "int";
        member.params = [];
        member.roles = [];
        return;
    }
    if (member.kind === "model") {
        member.type = "";
        member.params = [];
        member.roles = member.roles || [];
        return;
    }
    member.params = member.params || [];
    member.roles = [];
    if (member.kind === "signal") {
        member.type = "";
    }
}

function memberPanel(link, member, index, actions) {
    const box = tag("div", {class: "member"});
    const row = tag("div", {class: "member__row"});

    row.append(choice(KINDS, member.kind || "prop", (value) => {
        member.kind = value;
        settle(member);
        actions.rebuild();
    }));
    row.append(text(member.name, (value) => {
        member.name = value;
        actions.changed();
    }, "name"));

    if (member.kind === "prop") {
        row.append(choice(TYPES, member.type || "int", (value) => {
            member.type = value;
            actions.changed();
        }));
    }
    if (member.kind === "slot") {
        row.append(choice(["", ...TYPES], member.type, (value) => {
            member.type = value;
            actions.changed();
        }, "returns nothing"));
    }
    // Who reaches this member. Empty is whatever the point requires, which is the common
    // case; naming one here raises the bar for this member alone, and nothing about it
    // then crosses to a caller without that scope.
    row.append(choice(["", ...SCOPES], member.scope || "", (value) => {
        member.scope = value;
        actions.changed();
    }, "the point's scope"));
    row.append(remover(`Remove ${member.name || "this member"}`, () => {
        link.members.splice(index, 1);
        actions.rebuild();
    }));
    box.append(row);

    if (member.kind === "model") {
        box.append(partsPanel(member, "roles", "Roles", actions));
    }
    if (member.kind === "signal" || member.kind === "slot") {
        box.append(partsPanel(member, "params", "Parameters", actions));
    }
    return box;
}

function membersPanel(link, actions) {
    const box = tag("div", {class: "members"});
    box.append(tag("h2", {class: "members__title"}, "What crosses this link"));
    link.members = link.members || [];
    if (!link.members.length) {
        box.append(note("Nothing yet. A prop is owner state the consumer watches, a model "
                        + "is rows of it, a signal is one-way, and a slot is a call the "
                        + "owner answers with a Caller in hand.", true));
    }
    link.members.forEach((member, index) => {
        box.append(memberPanel(link, member, index, actions));
    });
    box.append(adder("Add member", () => {
        link.members.push({kind: "prop", name: "", type: "int", params: [], roles: []});
        actions.rebuild();
    }));
    box.append(note("Typing a property, a signal or a function into the owner's Source in "
                    + "the Files pane adds it here too. A model is the one kind only this "
                    + "panel can add: QML has no declaration form for one.", true));
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

function linkPanel(design, link, actions) {
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
                      + `${link.owner ? capitalised(link.owner) : "<Owner>"}, and it `
                      + "carries the "
                      + `${link.owner ? capitalised(link.owner) : "<Owner>"} type. `
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
    panel.append(field("Consumers", consumers));
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

    panel.append(membersPanel(link, actions));

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete connect point");
    remove.addEventListener("click", () => actions.removeLink(link));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
}

// Fill `host` with the panel for whatever is selected. `actions` is how the panel reports
// back: `changed` redraws, `rebuild` redraws and builds this panel again, `rename` carries a
// new name to the selection, and the two removers take the selection with them.
export function inspect(host, design, selected, actions) {
    host.replaceChildren();
    if (!selected) {
        host.append(tag("p", {class: "inspector__empty"},
                        "Pick an entity or a link to edit it. Drag from the handle on an "
                        + "entity's edge to another entity to draw a connect point between "
                        + "them, from the owner to the consumer."));
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
    if (link) {
        host.append(linkPanel(design, link, actions));
    }
}

export { TYPES, PROVIDER_FAMILIES };
