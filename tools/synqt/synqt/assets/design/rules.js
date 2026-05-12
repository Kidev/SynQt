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

// How many of an entity there are. One for everybody, or one per caller. It is the
// entity's answer and not a link's, because an entity is one thing everybody reaches or
// one thing per caller, and it cannot be both at once for two of its own surfaces.
// A client is never shared. It is one browser.

// One field says what an entity is, the same answer appmodel.entity_type gives. Defined
// here because this is the file with no DOM and no imports, so every other module can
// reach it and the node checker can run it outside a browser.
export function entityType(entity) {
    return String((entity && entity.type) || "") || "service";
}

// The scope vocabulary a scaffolded project starts with, in the order project.renderYaml
// writes it. Lowest authority first, which is the order a front's scope dots are stacked in
// and the order the member gate reads against.
export const SCOPES = ["anonymous", "user", "moderator", "admin"];

// The scopes one design names, which is the four above for a project that never said
// otherwise and the project's own where it did. A project may name its own (the arena
// tutorial gates its whole connect point on `player`), and a picker offering four words
// none of which the project uses is a picker that can only make the file wrong.
export function scopesOf(design) {
    const declared = design && design.scopes;
    const named = Array.isArray(declared) ? declared.map(String).filter(Boolean) : [];
    return named.length ? named : SCOPES;
}

// The scope a caller with no session holds. The one the project named, where it named one
// that is not the first of its order, and the first otherwise. Read the same way
// designdoc.scope_default_of writes it, so the pane and the disk agree about a default
// nobody typed.
export function scopeDefaultOf(design) {
    const scopes = scopesOf(design);
    const named = design && design.scopeDefault ? String(design.scopeDefault) : "";
    return scopes.includes(named) ? named : (scopes[0] || "anonymous");
}

// Which entity serves each scope on a point that is a front, `{}` when it is not one. The
// same reading appmodel.behind does, kept here so the canvas, the panel and the checker
// all decide what a front is the same way.
export function behindOf(link) {
    const declared = link && link.behind;
    if (!declared || typeof declared !== "object" || Array.isArray(declared)) {
        return {};
    }
    const found = {};
    for (const [scope, entity] of Object.entries(declared)) {
        if (String(scope) && String(entity)) {
            found[String(scope)] = String(entity);
        }
    }
    return found;
}

// Every entity that is a front, as the point it fronts and where each scope goes. A front is
// a web edge that owns a point it does not implement. It holds the session and the sign-in,
// and hands each caller to the entity that serves people of their scope. Keyed by entity,
// because that is what the canvas draws.
export function frontsOf(design) {
    const found = new Map();
    for (const link of linksOf(design)) {
        // The key is the declaration, the way `network:` works: writing `behind:` says this
        // point is answered by entities behind it, and what is under it says which. One with
        // nothing under it yet is a front nobody has wired, which is a state to draw rather
        // than a state to hide.
        if (isFront(link)) {
            // The project's own scopes travel with the front, because the seats on its back
            // are one per scope and a seat for a word the project does not use is a seat
            // that can only write a `behind:` line `synqt check` refuses. `seatsOfFront` is
            // read from three files and takes only the front, so this is where they arrive.
            found.set(String(link.owner || ""),
                      {link, tiers: behindOf(link), scopes: scopesOf(design)});
        }
    }
    return found;
}

// Every client an edge hands to a session that has signed in as nobody, keyed by the client
// and naming the edge that serves it. That client is the gate. It is what every visitor
// downloads before they are anybody, and the bundle behind it is a file their session cannot
// fetch at all. The drawing says so with a shape of its own, because a gate drawn as one more
// client disc is the one thing on the canvas whose whole job is invisible.
//
// Read from the edge's `bundles:`, which is where the fact is written, so an entity cannot be
// a gate by being named one.
export function gatesOf(design) {
    const found = new Map();
    for (const entity of entitiesOf(design)) {
        if (!isWebEdge(entity)) {
            continue;
        }
        const bundles = entity.bundles;
        if (!bundles || typeof bundles !== "object" || Array.isArray(bundles)) {
            continue;
        }
        const served = String(bundles.anonymous || "");
        // One bundle for everybody is the ordinary case and no gate at all. A gate is a
        // bundle that exists *because* a signed-in session gets a different one.
        if (served && Object.keys(bundles).length > 1) {
            found.set(served, nameOf(entity));
        }
    }
    return found;
}

// Does this entity run the sign-in flow? A web edge that says so, and nothing else can.
//
// It is the one switch in a project that decides whether anybody is ever anything but
// anonymous: the login, callback and logout routes are served by the edges that carry it,
// so `Session.login()` in a client reaches a route that exists only where this is on, and
// every scope, member gate and bundle mapping in the project hangs off that. The canvas
// draws a mark for it, the panel switches it, and the card says what it means. All three
// read it here, so they cannot disagree about which edge signs people in.
export function runsSignIn(entity) {
    return isWebEdge(entity || {}) && Boolean(entity && entity.identity);
}

// Is this point answered by entities behind it? The key being there is the answer.
export function isFront(link) {
    const declared = link && link.behind;
    return Boolean(declared) && typeof declared === "object" && !Array.isArray(declared);
}

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
    return entityType(entity || {}) === "web_edge";
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
    return repeats(linksOf(design).map((link) => String((link && link.owner) || ""))).map(
        (owner) => ({
            rule: "duplicate-link-owner",
            level: "error",
            link: owner,
            message: `'${owner}' has two connect points, and an entity has one. The later `
                + `one takes over the first, consumer list and export block together.`,
        }));
}

// A desktop-only client is left alone here, exactly as `synqt check` leaves it alone: it is
// not served by an edge, it dials the one build.desktop.edge_url names, and that edge can
// belong to another project. Drawing one is not a mistake to paint red.
function inBrowser(entity) {
    const targets = entity.targets && entity.targets.length ? entity.targets : ["wasm"];
    return targets.includes("wasm");
}

function clientWithoutEdge(design) {
    const entities = entitiesOf(design);
    if (entities.some(isWebEdge)) {
        return [];
    }
    return entities
        .filter((entity) => entityType(entity) === "client" && inBrowser(entity))
        .map((entity) => ({
            rule: "no-web-edge-for-client",
            level: "error",
            entity: nameOf(entity),
            message: `'${nameOf(entity)}' is a client and this project has no web edge for `
                + `it to connect to.`,
        }));
}

function linkFindings(design, link) {
    const found = [];
    const entities = entitiesOf(design);
    const known = new Set(entities.map(nameOf));
    const clients = new Set(entities.filter(
        (entity) => entityType(entity) === "client").map(nameOf));
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
    if (clients.has(owner)) {
        // An owner hosts the Source and listens for consumers to acquire it. A browser cannot
        // listen. There is no WebSocket server under WebAssembly, and the client is always the
        // one that connects out. Easy to draw by mistake now that a link can be pulled off any
        // side of a node, and impossible to build.
        found.push({
            rule: "client-owns-connect-point",
            level: "error",
            link: name,
            entity: owner,
            message: `'${owner}' is a client, so it cannot own '${name}': an owner listens for `
                + `consumers and a browser cannot listen. Draw this one from the web edge.`,
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

    // A scope belongs to a user's session, and only a browser caller has one. Gating a
    // member of a point no client consumes would refuse every caller that could ever reach
    // it, which looks like protection and is a member nobody can use.
    const gated = (Array.isArray(link && link.members) ? link.members : [])
        .filter((member) => String((member && member.scope) || ""));
    if (gated.length && !consumers.some((consumer) => clients.has(consumer))) {
        found.push({
            rule: "member-scope-without-a-browser",
            level: "error",
            link: name,
            message: `'${gated[0].name || "a member"}' on '${name}' is gated on a scope, and `
                + `no client consumes '${name}'. A calling entity has no session and so no `
                + `scope: gate it on Caller.entity in the slot instead.`,
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

// An entity nothing reaches and that reaches nothing. A warning, not an error: it is the
// state every entity passes through between being dropped on the canvas and being wired,
// and painting it red would mean the editor scolds you for the gesture it performed.
// The client and the edge are left out. Both have a browser to serve. So is a monitor: the
// link every service opens to it is derived from `monitoring.entity` rather than drawn, so
// an unwired-looking monitor is the wired state and not a missing line.
function orphanEntities(design) {
    return entitiesOf(design)
        .filter((entity) => entityType(entity) !== "client" && !isWebEdge(entity)
                            && entityType(entity) !== "monitor")
        .filter((entity) => !linksOf(design).some(
            (link) => link.owner === nameOf(entity)
                || (link.consumers || []).includes(nameOf(entity))))
        .map((entity) => ({
            rule: "orphan-entity",
            level: "warn",
            entity: nameOf(entity),
            message: `'${nameOf(entity)}' owns no connect point and consumes none, so `
                + `nothing can reach it and it can reach nothing. Draw a link to it, or `
                + `take it off the canvas.`,
        }));
}

// A project has one monitor. `monitoring.entity` names a single entity, and it is that line
// rather than any drawn link that makes every service report, so a second monitor beside it
// builds, starts, serves its console and stays empty. The first one on the canvas is the one
// the project wires. Every one after it is this.
function extraMonitors(design) {
    const monitors = entitiesOf(design).filter(
        (entity) => entityType(entity) === "monitor");
    return monitors.slice(1).map((entity) => ({
        rule: "second-monitor",
        level: "warn",
        entity: nameOf(entity),
        message: `'${nameOf(monitors[0])}' is already this project's monitor, and `
            + `monitoring.entity names one entity. Nothing would report to `
            + `'${nameOf(entity)}', so its history would stay empty.`,
    }));
}

// What turning an edge into a front does to everything already drawn.
//
// The switch is one click and it changes what the point means: the edge stops answering its
// own connect point, and each caller is served by whichever entity sits behind their scope
// instead. Everything below is a consequence of that click, painted the moment it happens
// rather than waiting for `synqt check` to say it after the project is downloaded. Each one
// mirrors a rule in synqt/check.py (`lint_fronts`), so the canvas and the command line
// never disagree about what a front is.
//
// Every one of them is marked on the connect point (`scope: "contract"`) and on the entity,
// not on the lines out of the point. A front that hands nobody anywhere is a fact about the
// point, and painting each consumer's line red said the lines were wrong when they are the
// only part of the picture that is right.
function frontFindings(design) {
    const found = [];
    const entities = entitiesOf(design);
    const known = new Map(entities.map((entity) => [nameOf(entity), entity]));
    const clients = new Set(entities.filter(
        (entity) => entityType(entity) === "client").map(nameOf));
    const owners = new Set(linksOf(design).map((link) => String(link.owner || "")));
    for (const [name, front] of frontsOf(design)) {
        const link = front.link;
        const owner = known.get(name);
        const wired = Object.entries(front.tiers).filter(([, tier]) => tier);
        if (owner && !isWebEdge(owner)) {
            found.push({
                rule: "front-is-not-an-edge",
                level: "error",
                entity: name,
                link: String(link.owner || ""),
                scope: "contract",
                message: `'${name}' hands its callers on and is not a web edge. A front `
                    + `terminates the browser link, holds the session and runs the sign-in `
                    + `before it hands anyone anywhere, and only a web edge does those.`,
            });
        }
        if (!consumersOf(link).some((consumer) => clients.has(consumer))) {
            found.push({
                rule: "front-without-a-browser",
                level: "error",
                entity: name,
                link: String(link.owner || ""),
                scope: "contract",
                message: `'${name}' hands its callers on and no client consumes it. A front `
                    + `splits browser callers by scope, and between entities there is no `
                    + `session to split on.`,
            });
        }
        // The one that catches an existing contract the moment the switch goes on: a slot
        // that answers a value resolves on the caller when the owner's slot returns, and a
        // front has no answer then. What it handed the call to is reached over the mesh and
        // replies later.
        for (const member of (link.members || [])) {
            if (member && member.kind === "slot" && member.type) {
                found.push({
                    rule: "front-cannot-answer",
                    level: "error",
                    link: String(link.owner || ""),
                    scope: "contract",
                    message: `'${member.name}' returns ${member.type}, and '${name}' hands `
                        + `its callers on, so it has no answer to give: what the call goes `
                        + `to is reached over the mesh and replies after the slot has `
                        + `returned. Make it return nothing and send the answer back with `
                        + `Caller.emit<Signal>.`,
                });
            }
        }
        for (const [scope, tier] of wired) {
            if (!known.has(tier)) {
                found.push({
                    rule: "front-tier-unknown",
                    level: "error",
                    entity: name,
                    link: String(link.owner || ""),
                    scope: "contract",
                    message: `'${name}' hands '${scope}' to '${tier}', which is not an `
                        + `entity in this project.`,
                });
            } else if (!owners.has(tier)) {
                found.push({
                    rule: "front-tier-owns-nothing",
                    level: "error",
                    entity: tier,
                    link: String(link.owner || ""),
                    scope: "contract",
                    message: `'${name}' hands '${scope}' to '${tier}', which owns no `
                        + `connect point, so there is nothing there to answer the calls.`,
                });
            }
        }
        // A warning and not an error. It is the state a front is in between the switch and
        // the first line drawn, the same way an entity is an orphan until it is wired.
        if (!wired.length) {
            found.push({
                rule: "front-hands-nobody",
                level: "warn",
                entity: name,
                link: String(link.owner || ""),
                scope: "contract",
                message: `'${name}' hands its callers to the entities behind it and nothing `
                    + `is behind it yet, so every caller is refused. Drag from a scope on `
                    + `its back to the entity that serves it, or turn the switch off and `
                    + `answer the point here.`,
            });
        }
    }
    return found;
}

// A client is one browser. There is nobody for it to be shared with, so the word says
// nothing there, and reading it in a project would teach the wrong thing about what it is
// for.
function sharedOnAClient(design) {
    return entitiesOf(design)
        .filter((entity) => entityType(entity) === "client" && entity.shared === true)
        .map((entity) => ({
            rule: "shared-on-a-client",
            level: "error",
            entity: nameOf(entity),
            message: `'${nameOf(entity)}' is the client and is marked shared. A client is `
                + `one browser and shares with nobody. Mark the edge instead if what you `
                + `meant is a Source per session.`,
        }));
}

// Every rule the page paints, over one design document. Entity-level findings first, then
// each link in the order it was drawn, so the list is stable between two runs on the same
// document and a reader can follow it down the canvas.
export function findings(design) {
    const found = [
        ...duplicateEntities(design),
        ...duplicateLinks(design),
        ...clientWithoutEdge(design),
        ...sharedOnAClient(design),
        ...frontFindings(design),
        ...orphanEntities(design),
        ...extraMonitors(design),
    ];
    for (const link of linksOf(design)) {
        found.push(...linkFindings(design, link));
    }
    return found;
}

