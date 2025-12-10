// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Reading a design back out of the configuration, so `synqt.yaml` in the files pane is a file
// somebody types into rather than a rendering they can only look at.
//
// The other direction is project.js, which writes the configuration from the document; these
// two are a pair and the suite holds them to it by writing a design, reading it back, and
// asserting the same design comes out.
//
// Deliberately not a YAML parser. It reads the shape SynQt writes and the shape SynQt asks
// for, which is a flat mapping of blocks, two lists of mappings, and one literal block; a
// general parser would accept anchors, flow mappings, multi-document streams and tags that
// the topology has no meaning for, and would then have to refuse them one at a time. What it
// does not understand it ignores, because the document models the topology and the contracts
// and nothing else: an edge's `tls:` block and the project's `scopes:` are real configuration
// that this page has no opinion about, and dropping them out of the *document* is not the
// same as dropping them out of the file (the server writes the topology back into the file it
// already has, and never rewrites the whole of it).
//
// Every refusal carries the line it happened on, because the reader is looking at that line.

// One line of the file, with its indentation measured and its comment taken off. Tabs are
// refused rather than counted: YAML does not allow them for indentation, and guessing a width
// for one is how a file reads correctly here and differently everywhere else.
function scan(text) {
    return String(text || "").split("\n").map((raw, index) => {
        const withoutComment = stripComment(raw);
        const body = withoutComment.trimEnd();
        const indent = body.length - body.trimStart().length;
        return {
            number: index + 1,
            raw,
            body: body.trim(),
            indent,
            tabbed: /^[ ]*\t/.test(raw),
            blank: body.trim() === "",
        };
    });
}

// A `#` comment, unless it is inside quotes. Nothing in this shape holds a `#` in an unquoted
// scalar, but a project name in quotes may, and taking it off would rename the project.
function stripComment(line) {
    let quote = "";
    for (let at = 0; at < line.length; at += 1) {
        const character = line[at];
        if (quote) {
            if (character === quote) {
                quote = "";
            }
            continue;
        }
        if (character === '"' || character === "'") {
            quote = character;
            continue;
        }
        if (character === "#" && (at === 0 || /\s/.test(line[at - 1]))) {
            return line.slice(0, at);
        }
    }
    return line;
}

export class YamlError extends Error {
    constructor(line, message) {
        super(line ? `line ${line}: ${message}` : message);
        this.line = line || 0;
    }
}

// `key: value`, or null when the line is not one. The value may be empty, which is how a block
// opens.
function keyed(line) {
    const found = line.body.match(/^(-\s+)?([A-Za-z_][\w.-]*)\s*:(?:\s+(.*))?$/);
    if (!found) {
        return null;
    }
    return {
        dash: Boolean(found[1]),
        key: found[2],
        value: (found[3] || "").trim(),
        // Where the key itself starts, which is what a nested block is measured against: on a
        // list item the dash is part of the indentation of everything under it.
        indent: line.indent + (found[1] ? found[1].length : 0),
    };
}

function scalar(text, line) {
    const value = String(text || "").trim();
    if ((value.startsWith('"') && value.endsWith('"') && value.length > 1)
        || (value.startsWith("'") && value.endsWith("'") && value.length > 1)) {
        try {
            return value[0] === '"' ? JSON.parse(value) : value.slice(1, -1);
        } catch (error) {
            throw new YamlError(line, `${value} is not a string this reads`);
        }
    }
    return value;
}

function flowList(text, line) {
    const value = String(text || "").trim();
    if (!value.startsWith("[") || !value.endsWith("]")) {
        throw new YamlError(line, `expected a list in [brackets], found ${value || "nothing"}`);
    }
    const inner = value.slice(1, -1).trim();
    return inner ? inner.split(",").map((part) => scalar(part, line)).filter(Boolean) : [];
}

function truthy(text) {
    return ["true", "yes", "on", "1"].includes(String(text || "").trim().toLowerCase());
}

// The block of lines belonging to the mapping that opened at `lines[from]`: everything under
// it that is indented past `indent`, blank lines included.
function blockAfter(lines, from, indent) {
    let end = from + 1;
    while (end < lines.length && (lines[end].blank || lines[end].indent > indent)) {
        end += 1;
    }
    return {body: lines.slice(from + 1, end), next: end};
}

// A `|` literal block, given back with its own indentation taken off and its comment lines
// left alone: an `export:` block is contract source, where `//` is a comment and `#` is not.
function literalAfter(lines, from, indent) {
    let end = from + 1;
    while (end < lines.length && (lines[end].blank || lines[end].indent > indent)) {
        end += 1;
    }
    const held = lines.slice(from + 1, end).filter((line) => !line.blank);
    if (!held.length) {
        return {text: "", next: end};
    }
    const least = Math.min(...held.map((line) => line.indent));
    const text = lines.slice(from + 1, end)
        .map((line) => (line.blank ? "" : line.raw.slice(least)))
        .join("\n");
    return {text, next: end};
}

// Every `- ` item of a list, each as the lines that make it up.
function itemsOf(body) {
    const items = [];
    let current = null;
    for (const line of body) {
        if (line.blank) {
            if (current) {
                current.push(line);
            }
            continue;
        }
        if (/^-\s/.test(line.body) || line.body === "-") {
            current = [line];
            items.push(current);
            continue;
        }
        if (!current) {
            throw new YamlError(line.number, `'${line.body}' is not under a list item`);
        }
        current.push(line);
    }
    return items;
}

// The top-level blocks of the file, by key.
function topLevel(lines) {
    const blocks = new Map();
    let at = 0;
    while (at < lines.length) {
        const line = lines[at];
        if (line.blank) {
            at += 1;
            continue;
        }
        if (line.tabbed) {
            throw new YamlError(line.number, "indented with a tab; YAML wants spaces");
        }
        if (line.indent !== 0) {
            throw new YamlError(line.number, `'${line.body}' is indented under nothing`);
        }
        const pair = keyed(line);
        if (!pair) {
            throw new YamlError(line.number, `'${line.body}' is not 'key: value'`);
        }
        const {body, next} = blockAfter(lines, at, 0);
        blocks.set(pair.key, {value: pair.value, body, line: line.number});
        at = next;
    }
    return blocks;
}

// One mapping's own keys, from the lines of a list item or a nested block. Nested blocks come
// back as their lines so a caller can go one level further. Scalars come back read.
function mapping(body) {
    const found = new Map();
    let at = 0;
    // A list item's first line carries the dash, so everything in that item is measured from
    // where the key after the dash starts.
    const base = body.length ? (keyed(body[0]) || {indent: body[0].indent}).indent : 0;
    while (at < body.length) {
        const line = body[at];
        if (line.blank) {
            at += 1;
            continue;
        }
        if (line.tabbed) {
            throw new YamlError(line.number, "indented with a tab; YAML wants spaces");
        }
        const pair = keyed(line);
        if (!pair || pair.indent !== base) {
            at += 1;
            continue;                   // deeper than this mapping, or not a key, so not this mapping's
        }
        if (pair.value === "|" || pair.value === "|-") {
            const {text, next} = literalAfter(body, at, base);
            found.set(pair.key, {literal: text, line: line.number});
            at = next;
            continue;
        }
        const {body: nested, next} = blockAfter(body, at, base);
        found.set(pair.key, {value: pair.value, body: nested, line: line.number});
        at = next;
    }
    return found;
}

const TYPE = "[A-Za-z_]\\w*(?:\\[\\d+\\])?";

function paramsOf(text, line) {
    const inner = String(text || "").trim();
    if (!inner) {
        return [];
    }
    return inner.split(",").map((part) => {
        const found = part.trim().match(new RegExp(`^(${TYPE})\\s+([A-Za-z_]\\w*)$`));
        if (!found) {
            throw new YamlError(line, `'${part.trim()}' is not '<type> <name>'`);
        }
        return {type: found[1], name: found[2]};
    });
}

// One line of an `export:` block, as the member record the document holds. A line that names a
// member and nothing else is read through what the owner declares, which is exactly what the
// command line does with one. Where the owner declares no such thing the type is `var`, the
// same answer `synqt infer` writes when nothing gave it away.
export function memberFrom(text, line, declared) {
    const written = String(text || "").replace(/\/\/.*$/, "").trim();
    if (!written) {
        return null;
    }
    // The scope gate a member may open with, taken off before the member is read: who may
    // reach it is a separate question from what it is, and every form below is the same
    // with or without one.
    const gated = written.match(/^<\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*>\s*(.*)$/);
    const scope = gated ? gated[1].split(",").map((name) => name.trim()).join(",") : "";
    const body = gated ? gated[2].trim() : written;
    const gate = (member) => (member ? Object.assign(member, {scope}) : member);
    const prop = body.match(new RegExp(`^prop\\s+(${TYPE})\\s+([A-Za-z_]\\w*)$`));
    if (prop) {
        return gate({kind: "prop", name: prop[2], type: prop[1], params: [], roles: []});
    }
    const model = body.match(/^model\s+([A-Za-z_]\w*)\s*\((.*)\)$/);
    if (model) {
        return gate({kind: "model", name: model[1], type: "", params: [],
                     roles: paramsOf(model[2], line)});
    }
    const signal = body.match(/^signal\s+([A-Za-z_]\w*)\s*\((.*)\)$/);
    if (signal) {
        return gate({kind: "signal", name: signal[1], type: "",
                     params: paramsOf(signal[2], line), roles: []});
    }
    const slot = body.match(new RegExp(`^slot\\s+(?:(${TYPE})\\s+)?([A-Za-z_]\\w*)\\s*\\((.*)\\)$`));
    if (slot) {
        return gate({kind: "slot", name: slot[2], type: slot[1] || "",
                     params: paramsOf(slot[3], line), roles: []});
    }
    const bare = body.match(/^([A-Za-z_]\w*)$/);
    if (bare) {
        const known = (declared || []).find((one) => one.name === bare[1]);
        return gate(known
            ? {...known, line: undefined}
            : {kind: "prop", name: bare[1], type: "var", params: [], roles: []});
    }
    throw new YamlError(line, `'${body}' is not a member: try 'prop int count', `
        + "'model rows(int id)', 'signal changed(int to)' or 'slot act(string what)'");
}

function membersFrom(literal, from, declared) {
    const members = [];
    String(literal || "").split("\n").forEach((text, offset) => {
        const member = memberFrom(text, from + offset + 1, declared);
        if (member) {
            members.push(member);
        }
    });
    return members;
}

function entityFrom(item) {
    const fields = mapping(item);
    const nameAt = fields.get("name");
    if (!nameAt || !nameAt.value) {
        throw new YamlError(item.length ? item[0].number : 0, "an entity with no name");
    }
    const entity = {name: scalar(nameAt.value, nameAt.line)};
    const type = fields.get("type");
    entity.type = type ? scalar(type.value, type.line) : "service";
    const identity = fields.get("identity");
    if (identity && truthy(identity.value)) {
        entity.identity = true;
    }
    const shared = fields.get("shared");
    if (shared && shared.value !== "") {
        entity.shared = truthy(shared.value);
    }
    const targets = fields.get("targets");
    if (targets && targets.value) {
        entity.targets = flowList(targets.value, targets.line);
    }
    const provider = fields.get("provider");
    if (provider) {
        // Either `provider: sqlite` or the block form the scaffold writes.
        if (provider.value) {
            entity.provider = scalar(provider.value, provider.line);
        } else {
            const inner = mapping(provider.body);
            const named = inner.get("name");
            if (named) {
                entity.provider = scalar(named.value, named.line);
            }
        }
    }
    return entity;
}

function linkFrom(item, entities) {
    const fields = mapping(item);
    const nameAt = fields.get("name");
    if (nameAt) {
        throw new YamlError(nameAt.line, "a connect point is not named; its owner names it");
    }
    const owner = fields.get("owner");
    if (!owner || !owner.value) {
        throw new YamlError(item.length ? item[0].number : 0, "a connect point with no owner");
    }
    // An entity has one connect point, so the owner names it. The accessor consumers read,
    // the contract, and the file that implements it all come from this one word.
    const link = {owner: scalar(owner.value, owner.line)};
    link.name = link.owner;
    const consumers = fields.get("consumers");
    link.consumers = consumers && consumers.value ? flowList(consumers.value, consumers.line)
                                                  : [];
    const transport = fields.get("transport");
    if (transport && transport.value) {
        link.transport = scalar(transport.value, transport.line);
    }
    const scope = fields.get("scope");
    if (scope && scope.value) {
        link.scope = scalar(scope.value, scope.line);
    }
    // Which entity serves each scope, on a point that is a front. Read as a mapping of its
    // own rather than folded into the link's fields, because the keys are scope names and
    // any of them could collide with a field name here.
    const behind = fields.get("behind");
    if (behind && Array.isArray(behind.body) && behind.body.length) {
        const tiers = {};
        for (const [name, held] of mapping(behind.body)) {
            if (held && held.value) {
                tiers[name] = scalar(held.value, held.line);
            }
        }
        link.behind = tiers;
    }
    const exported = fields.get("export");
    if (exported && typeof exported.literal === "string") {
        const declared = (entities.get(link.owner) || {}).declared || [];
        link.members = membersFrom(exported.literal, exported.line, declared);
    }
    return link;
}

// The design `text` describes, over `held`: the document the page already has, which is where
// everything the configuration does not carry comes from. Positions, the QML each entity and
// each Source holds, and the source hash all belong to the page and not to this file, so an
// entity that was already there keeps them and a new one gets none.
//
// Throws YamlError, with the line, for anything it cannot read.
export function parseDesign(text, held, declarationsOf) {
    const lines = scan(text);
    const blocks = topLevel(lines);
    const design = {
        version: 1,
        project: held && held.project ? held.project : "",
        sourceHash: (held || {}).sourceHash || "",
        entities: [],
        links: [],
    };
    const project = blocks.get("project");
    if (project) {
        const named = mapping(project.body).get("name");
        if (named && named.value) {
            design.project = scalar(named.value, named.line);
        }
    }
    const kept = new Map(((held || {}).entities || []).map((one) => [one.name, one]));
    const keptLinks = new Map(((held || {}).links || []).map((one) => [one.name, one]));

    const entities = blocks.get("entities");
    for (const item of entities ? itemsOf(entities.body) : []) {
        const read = entityFrom(item);
        const before = kept.get(read.name) || {};
        design.entities.push({
            ...read,
            x: before.x,
            y: before.y,
            qml: before.qml,
        });
    }
    // What each owner declares, so a member named on its own in an export block reads as the
    // thing the owner already has rather than as an untyped guess.
    const byName = new Map(design.entities.map((entity) => [entity.name, {
        ...entity,
        declared: declarationsOf ? declarationsOf(entity) : [],
    }]));

    const points = blocks.get("connect_points");
    for (const item of points ? itemsOf(points.body) : []) {
        const read = linkFrom(item, byName);
        const before = keptLinks.get(read.name) || {};
        design.links.push({
            slot: before.slot,
            server: before.server,
            qml: before.qml,
            members: before.members || [],
            ...read,
        });
    }
    return design;
}
