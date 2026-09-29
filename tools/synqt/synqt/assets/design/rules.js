// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What the editor paints while you draw, so a line that cannot work says so as you draw it,
// not after you apply it.
//
// A subset of `synqt check`, never a second opinion on it. Every rule here has a topology in
// topologies.json; the Python suite asserts `synqt check` reaches the same verdict at the
// same level for each one, and the node checker asserts this file does.
//
// Pure functions over the document, no DOM, so the node checker can import this file.

// One field says what an entity is, the same answer appmodel.entity_type gives. Defined here
// because this file has no DOM and no imports, so every other module can reach it.
export function entityType(entity) {
    return String((entity && entity.type) || "") || "service";
}

// The names appmodel.is_valid_project_name and is_valid_entity_name accept. An entity name
// becomes a directory, a build target, an accessor and a certificate's subject; a project
// name becomes the CMake project, the client's QML module and the container names.
const NAME_PATTERN = /^[A-Za-z][A-Za-z0-9_-]*$/;
const NAME_MAX = 64;
const RESERVED_ENTITY_NAMES = ["ca", "docker-ca"];

// Why `name` cannot name a project, or "" when it can.
export function projectNameProblem(name) {
    const text = String(name || "");
    if (!text) {
        return "A project needs a name.";
    }
    if (text.length > NAME_MAX || !NAME_PATTERN.test(text)) {
        return `'${text.slice(0, 80)}' cannot name a project: a project name starts with a `
            + `letter and is made of letters, digits, underscores and hyphens, up to `
            + `${NAME_MAX} characters.`;
    }
    return "";
}

// Why `name` cannot name an entity, or "" when it can.
export function entityNameProblem(name) {
    const text = String(name || "");
    if (!text) {
        return "An entity needs a name.";
    }
    if (RESERVED_ENTITY_NAMES.includes(text.toLowerCase())) {
        return `'${text}' is taken: synqt/mesh/ holds '${text.toLowerCase()}.crt' and `
            + `'${text.toLowerCase()}.key' for the project's own certificate authority.`;
    }
    if (text.length > NAME_MAX || !NAME_PATTERN.test(text)) {
        return `'${text.slice(0, 80)}' cannot name an entity: an entity name starts with a `
            + `letter and is made of letters, digits, underscores and hyphens, up to `
            + `${NAME_MAX} characters.`;
    }
    return "";
}

// The scope vocabulary a scaffolded project starts with, in the order project.renderYaml
// writes it: lowest authority first, the order a front's scope seats are stacked in.
export const SCOPES = ["anonymous", "user", "moderator", "admin"];

// The scopes one design names: the project's own where it declares them (the arena tutorial
// gates its connect point on `player`), the four above otherwise.
export function scopesOf(design) {
    const declared = design && design.scopes;
    const named = Array.isArray(declared) ? declared.map(String).filter(Boolean) : [];
    return named.length ? named : SCOPES;
}

// The scope a caller with no session holds: the one the project named, else the first of
// its order. Read as designdoc.scope_default_of writes it, so the pane and the disk agree.
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
        // The key is the declaration, as with `network:`: writing `behind:` says entities
        // behind this point answer it, and what is under it says which. One with nothing
        // under it yet is a front nobody has wired, and it is drawn.
        if (isFront(link)) {
            // The project's scopes travel with the front, one seat per scope, so no seat
            // names a scope `synqt check` would refuse. `seatsOfFront` takes only the front.
            found.set(String(link.owner || ""),
                      {link, tiers: behindOf(link), scopes: scopesOf(design)});
        }
    }
    return found;
}

// Every client an edge hands to an anonymous session, keyed by the client and naming the edge
// that serves it. That client is the gate: every visitor downloads it before signing in, and
// their session cannot fetch the bundle behind it. The canvas draws it with a shape of its
// own. Read from the edge's `bundles:`, so an entity cannot be a gate by its name.
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
        // One bundle for everybody is no gate. A gate is the anonymous bundle of an edge that
        // gives a signed-in session a different one.
        if (served && Object.keys(bundles).length > 1) {
            found.set(served, nameOf(entity));
        }
    }
    return found;
}

// Does this entity run the sign-in flow? A web edge that says so, and nothing else can.
//
// The login, callback and logout routes are served only by the edges that carry it, so
// without it nobody is ever anything but anonymous. The canvas mark, the panel switch and the
// card all read it here.
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

// The names declared more than once, in the order they were first declared. The build keys
// both lists by name, so the later entry wins and the earlier one is never built.
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

function unusableEntityNames(design) {
    return entitiesOf(design)
        .filter((entity) => entityNameProblem(nameOf(entity)))
        .map((entity) => ({
            rule: "entity-name-unusable",
            level: "error",
            entity: nameOf(entity),
            message: entityNameProblem(nameOf(entity)),
        }));
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

// A desktop-only client is left alone, as `synqt check` leaves it: no edge serves it, it
// dials the one build.desktop.edge_url names, and that edge can belong to another project.
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
        // An owner hosts the Source and listens for consumers. A browser cannot listen: there
        // is no WebSocket server under WebAssembly, and the client always connects out.
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

    // A scope belongs to a user's session, and only a browser caller has one, so a gated
    // member on a point no client consumes refuses every caller that can reach it.
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

    // A warning: on a local socket the operating system identifies the connecting user, not
    // the entity, so any process running as that user can present any entity name.
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

// An entity nothing reaches and that reaches nothing. A warning, since every entity is in this
// state between being dropped on the canvas and being wired. The client and the edge serve
// a browser, so they are left out, and so is a monitor: the link every service opens to it
// comes from `monitoring.entity`, not from a drawn line.
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

// A project has one monitor. `monitoring.entity` names a single entity and makes every service
// report to it, so a second monitor builds, starts, serves its console and stays empty. The
// first one on the canvas is the one the project wires.
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

// A link a monitor consumes, which the editor will not draw and a hand-written `synqt.yaml`
// can still hold. Reported, not dropped on opening, so opening a project never changes it.
//
// Only the consuming half, the half `synqt check` refuses too: entities report to a monitor
// and a monitor reaches none of them. A point the monitor owns is left alone here as the CLI
// leaves it. Neither can be drawn; see canvas.linkRefusal.
function monitorAsConsumer(design) {
    const monitors = new Set(entitiesOf(design)
        .filter((entity) => entityType(entity) === "monitor")
        .map((entity) => nameOf(entity)));
    if (monitors.size === 0) {
        return [];
    }
    const found = [];
    for (const link of linksOf(design)) {
        for (const consumer of link.consumers || []) {
            if (!monitors.has(consumer)) {
                continue;
            }
            found.push({
                rule: "monitor-as-consumer",
                level: "error",
                link: link.name,
                message: `'${consumer}' is a monitor, and a monitor consumes nothing: `
                    + `entities report to it, and the one link it has comes from `
                    + `monitoring.entity. Take it off this point's consumers.`,
            });
        }
    }
    return found;
}

// What turning an edge into a front does to everything already drawn.
//
// The switch changes what the point means: the edge stops answering its own connect point,
// and each caller is served by the entity behind their scope. Each finding below mirrors a
// rule in synqt/check.py (`lint_fronts`) and is painted as soon as the switch is flipped.
//
// Every one is marked on the connect point (`scope: "contract"`) and on the entity, not on the
// lines out of the point, which are not what is wrong.
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
        // A slot that returns a value resolves on the caller when the owner's slot returns,
        // and a front has no answer then: the entity it hands the call to replies later, over
        // the mesh.
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
        // A warning: a front is in this state between the switch and the first line drawn.
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

// A client is one browser and shares with nobody, so `shared` says nothing there, whichever
// way it is written.
function sharedOnAClient(design) {
    return entitiesOf(design)
        .filter((entity) => entityType(entity) === "client" && typeof entity.shared === "boolean")
        .map((entity) => ({
            rule: "shared-on-a-client",
            level: "error",
            entity: nameOf(entity),
            message: `'${nameOf(entity)}' is the client and sets 'shared'. A client is one `
                + `browser and shares with nobody. Write 'shared: false' on the edge if what `
                + `you meant is a Source per session.`,
        }));
}

// Every rule the page paints, over one design document: entity findings first, then each
// link in the order it was drawn, so the list is stable between runs.
export function findings(design) {
    const found = [
        ...unusableEntityNames(design),
        ...duplicateEntities(design),
        ...duplicateLinks(design),
        ...clientWithoutEdge(design),
        ...sharedOnAClient(design),
        ...frontFindings(design),
        ...orphanEntities(design),
        ...extraMonitors(design),
        ...monitorAsConsumer(design),
    ];
    for (const link of linksOf(design)) {
        found.push(...linkFindings(design, link));
    }
    return found;
}

