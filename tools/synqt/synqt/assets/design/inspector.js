// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The panel for whatever is selected: an entity's own fields, or a connect point's, and
// under a connect point the members of the contract that crosses it.
//
// Every control writes straight into the document and asks the page to redraw. Two of them
// do more than they appear to: renaming an entity carries the new name into every connect
// point that referred to the old one, and deleting one takes the connect points it owned
// with it, because leaving either behind would leave the project naming an entity that is
// not there.

import { INSTANCE_MODES } from "./rules.js";

// The .syn type vocabulary, from synqtc/types.py. `var` is in it because a model role may
// carry anything, and the roles are where that comes up.
const TYPES = ["int", "string", "bool", "real", "float", "double", "var"];

const KINDS = ["prop", "model", "signal", "slot"];

// The blueprints an entity may be built from, and the three that take a data provider
// (addentity.BLUEPRINTS). A gateway and a jobs entity have no engine behind them.
const BLUEPRINTS = ["persistence", "cache", "document", "gateway", "jobs", "service"];
const PROVIDER_FAMILIES = new Set(["persistence", "cache", "document"]);

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

function field(label, control) {
    const wrap = tag("label", {class: "field"});
    wrap.append(tag("span", {class: "field__label"}, label), control);
    return wrap;
}

function note(text) {
    return tag("p", {class: "field__note"}, text);
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
    const panel = document.createDocumentFragment();
    panel.append(tag("h2", {class: "inspector__title"}, entity.name || "this entity"));

    panel.append(field("Name", text(entity.name, (value) => {
        renameEntity(design, entity, value);
        actions.rename("entity", value);
    })));

    panel.append(field("Kind", choice(["client", "service"], entity.kind || "service",
                                      (value) => {
        entity.kind = value;
        if (value === "client") {
            entity.capability = "";
            entity.blueprint = "";
            entity.provider = "";
            entity.identity = false;
            entity.targets = entity.targets && entity.targets.length
                ? entity.targets : ["wasm"];
        } else {
            entity.targets = [];
        }
        actions.rebuild();
    })));

    if ((entity.kind || "service") === "client") {
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
    } else {
        panel.append(field("Capability", choice(["", "web_edge"], entity.capability,
                                                (value) => {
            entity.capability = value;
            if (value === "web_edge") {
                entity.blueprint = "";
                entity.provider = "";
            }
            actions.rebuild();
        }, "none (a mesh service)")));

        if (entity.capability === "web_edge") {
            panel.append(check("Runs the sign-in flow", Boolean(entity.identity), (on) => {
                entity.identity = on;
                actions.changed();
            }));
            panel.append(note("The one entity the browser can reach. Everything else is "
                              + "behind it, on the mesh."));
        } else {
            panel.append(field("Blueprint", choice(["", ...BLUEPRINTS], entity.blueprint,
                                                   (value) => {
                entity.blueprint = value;
                if (!PROVIDER_FAMILIES.has(value)) {
                    entity.provider = "";
                }
                actions.rebuild();
            }, "none (write it yourself)")));

            if (PROVIDER_FAMILIES.has(entity.blueprint)) {
                panel.append(field("Provider", text(entity.provider, (value) => {
                    entity.provider = value;
                    actions.changed();
                }, "sqlite")));
                panel.append(note("The engine behind the blueprint. Its credentials come "
                                  + "from this entity's own environment and never from "
                                  + "here."));
            }
        }
    }

    const actionsRow = tag("div", {class: "inspector__actions"});
    const remove = tag("button", {type: "button", class: "button button--danger"},
                       "Delete entity");
    remove.addEventListener("click", () => actions.removeEntity(entity));
    actionsRow.append(remove);
    panel.append(actionsRow);
    return panel;
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
    box.append(tag("h2", {}, "What crosses this link"));
    link.members = link.members || [];
    if (!link.members.length) {
        box.append(note("Nothing yet. A prop is owner state the consumer watches, a model "
                        + "is rows of it, a signal is one-way, and a slot is a call the "
                        + "owner answers with a Caller in hand."));
    }
    link.members.forEach((member, index) => {
        box.append(memberPanel(link, member, index, actions));
    });
    box.append(adder("Add member", () => {
        link.members.push({kind: "prop", name: "", type: "int", params: [], roles: []});
        actions.rebuild();
    }));
    return box;
}

function linkPanel(design, link, actions) {
    const panel = document.createDocumentFragment();
    panel.append(tag("h2", {class: "inspector__title"}, link.name || "this connect point"));

    panel.append(field("Name", text(link.name, (value) => {
        link.name = value;
        actions.rename("link", value);
    })));
    panel.append(field("Contract", text(link.contract, (value) => {
        link.contract = value;
        actions.changed();
    }, "Auction")));

    const names = (design.entities || []).map((entity) => entity.name);
    panel.append(field("Owner", choice(["", ...names], link.owner, (value) => {
        link.owner = value;
        link.consumers = (link.consumers || []).filter((consumer) => consumer !== value);
        actions.rebuild();
    }, "nobody yet")));

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

    panel.append(field("Instance", choice(INSTANCE_MODES, link.instance || "shared",
                                          (value) => {
        link.instance = value;
        actions.changed();
    })));
    panel.append(note("shared is one Source for everybody. per_session is one per browser "
                      + "connection, which is what gives a slot its Caller; per_peer is one "
                      + "per calling entity."));

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

export { TYPES, BLUEPRINTS, PROVIDER_FAMILIES };
