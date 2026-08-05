// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What the editor paints while you are drawing, so a line that cannot work says so as you
// move it rather than after you apply it.
//
// This is a subset of `synqt check` and never a second opinion on it. Every rule here has a
// topology in topologies.json, the Python suite asserts `synqt check` reaches the same
// verdict at the same level for each one, and the node checker asserts this file does. What
// the build reads is synqt.yaml, and the page never gets to disagree with the command line
// about what is in it.
//
// Pure functions over the document, no DOM: the node checker imports this file directly, and
// a rule that reached for the page could not be checked outside a browser.

const INSTANCE_MODES = ["shared", "per_session", "per_peer"];

function entitiesOf(design) {
    return Array.isArray(design && design.entities) ? design.entities : [];
}

function linksOf(design) {
    return Array.isArray(design && design.links) ? design.links : [];
}

function nameOf(node) {
    return String((node && node.name) || "");
}

function consumersOf(link) {
    return Array.isArray(link && link.consumers) ? link.consumers.map(String) : [];
}

function isWebEdge(entity) {
    return String((entity && entity.capability) || "") === "web_edge";
}

// The names declared more than once, in the order they were first declared. Both maps are
// keyed by name, so a repeat is not a collision anyone is told about: the later entry wins
// and the earlier one is never built.
function repeats(names) {
    const seen = new Set();
    const twice = [];
    for (const name of names) {
        if (!name) {
            continue;
        }
        if (seen.has(name) && !twice.includes(name)) {
            twice.push(name);
        }
        seen.add(name);
    }
    return twice;
}

function duplicateEntities(design) {
    return repeats(entitiesOf(design).map(nameOf)).map((name) => ({
        rule: "duplicate-entity-name",
        level: "error",
        entity: name,
        message: `Two entities are named '${name}'. The later one wins and the earlier one `
            + `is never built.`,
    }));
}

function duplicateLinks(design) {
    return repeats(linksOf(design).map(nameOf)).map((name) => ({
        rule: "duplicate-link-name",
        level: "error",
        link: name,
        message: `Two connect points are named '${name}'. The later one takes over the `
            + `first, owner and consumer list together.`,
    }));
}

function clientWithoutEdge(design) {
    const entities = entitiesOf(design);
    if (entities.some(isWebEdge)) {
        return [];
    }
    return entities.filter((entity) => entity.kind === "client").map((entity) => ({
        rule: "no-web-edge-for-client",
        level: "error",
        entity: nameOf(entity),
        message: `'${nameOf(entity)}' is a client and this project has no web edge for it to `
            + `connect to.`,
    }));
}

function linkFindings(design, link) {
    const found = [];
    const entities = entitiesOf(design);
    const known = new Set(entities.map(nameOf));
    const clients = new Set(entities.filter((e) => e.kind === "client").map(nameOf));
    const edges = new Set(entities.filter(isWebEdge).map(nameOf));
    const name = nameOf(link);
    const owner = String((link && link.owner) || "");
    const consumers = consumersOf(link);

    if (!known.has(owner)) {
        found.push({
            rule: "unknown-owner",
            level: "error",
            link: name,
            message: `'${name}' is owned by '${owner}', which is not an entity in this `
                + `project, so nothing would host it.`,
        });
    }
    if (consumers.includes(owner)) {
        found.push({
            rule: "owner-is-its-own-consumer",
            level: "error",
            link: name,
            message: `'${owner}' owns '${name}', so it holds the Source and does not acquire `
                + `a replica of it.`,
        });
    }
    for (const consumer of consumers) {
        if (!known.has(consumer)) {
            found.push({
                rule: "unknown-consumer",
                level: "error",
                link: name,
                entity: consumer,
                message: `'${name}' lists '${consumer}' as a consumer, which is not an `
                    + `entity in this project.`,
            });
        } else if (clients.has(consumer) && !edges.has(owner)) {
            // The browser holds no mesh certificate and cannot route to the mesh. A client
            // reaches a web edge or it reaches nothing.
            found.push({
                rule: "client-consumes-non-edge",
                level: "error",
                link: name,
                entity: consumer,
                message: `'${consumer}' is a client and '${name}' is owned by '${owner}', `
                    + `which is not a web edge. The browser can only reach a web edge.`,
            });
        }
    }

    const instance = link && link.instance;
    if (instance && !INSTANCE_MODES.includes(String(instance))) {
        found.push({
            rule: "invalid-instance",
            level: "error",
            link: name,
            message: `'${name}' has instance '${instance}'. It must be one of `
                + `${INSTANCE_MODES.join(", ")}, and anything else is built as one shared `
                + `Source for every caller.`,
        });
    }

    // Not a mistake, and not silent either. On a local socket the operating system
    // identifies the connecting user, not the entity, so any process running as that user
    // can present any entity name.
    if (String((link && link.transport) || "") === "local") {
        found.push({
            rule: "local-transport-declared",
            level: "warn",
            link: name,
            message: `'${name}' is on a local socket, so its caller entity is trusted by `
                + `colocation rather than by certificate. Gate a privileged action on `
                + `Caller.isEntityVerified.`,
        });
    }
    return found;
}

// Every rule the page paints, over one design document. Entity-level findings first, then
// each link in the order it was drawn, so the list is stable between two runs on the same
// document and a reader can follow it down the canvas.
export function findings(design) {
    const found = [
        ...duplicateEntities(design),
        ...duplicateLinks(design),
        ...clientWithoutEdge(design),
    ];
    for (const link of linksOf(design)) {
        found.push(...linkFindings(design, link));
    }
    return found;
}

export { INSTANCE_MODES };
