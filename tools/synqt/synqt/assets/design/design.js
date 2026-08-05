// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The editor: one design document, the canvas that draws it, the panel that edits it, and
// the two requests that turn it into files.
//
// Nothing here writes to the project. Editing changes a document held in this tab; Review
// asks the server what applying it would do and shows the diff; Apply names the change set
// that was shown, by its digest, and the server refuses anything else. The rules the page
// paints while you drag are rules.js, a subset of `synqt check` that the suite holds to the
// same verdicts, and the verdict that decides is the one the server returns.
//
// Run with no server behind it (the copy on synqt.org) the page still edits, and Apply
// becomes a download of the project it would have written.

import { findings as ruleFindings } from "./rules.js";
import { draw, element, entityAt, roleOf } from "./canvas.js";
import { inspect } from "./inspector.js";
import { projectFiles } from "./project.js";
import { zipBytes } from "./zip.js";

// The three columns a topology reads in, the same ones designdoc.py lays a project out in:
// the browser on the left, the edge it reaches in the middle, and everything it must not
// reach on the right.
const COLUMNS = {client: 40, edge: 360, service: 680};
const FIRST_Y = 40;
const ROW_HEIGHT = 160;

const ZOOM_RANGE = [0.35, 2.4];

// Far enough that a click with a shaking hand is still a click and not a drag.
const DRAG_SLOP = 3;

const PALETTE = [
    {label: "Client", role: "client", base: "client",
     make: () => ({kind: "client", targets: ["wasm"]})},
    {label: "Web edge", role: "edge", base: "web",
     make: () => ({kind: "service", capability: "web_edge"})},
    {label: "Persistence", role: "persistence", base: "database",
     make: () => ({kind: "service", blueprint: "persistence", provider: "sqlite"})},
    {label: "Cache", role: "cache", base: "cache",
     make: () => ({kind: "service", blueprint: "cache", provider: "memory"})},
    {label: "Document store", role: "document", base: "documents",
     make: () => ({kind: "service", blueprint: "document", provider: "memory"})},
    {label: "Gateway", role: "gateway", base: "gateway",
     make: () => ({kind: "service", blueprint: "gateway"})},
    {label: "Jobs", role: "jobs", base: "jobs",
     make: () => ({kind: "service", blueprint: "jobs"})},
    {label: "Service", role: "service", base: "service",
     make: () => ({kind: "service"})},
];

const state = {
    design: {version: 1, project: "", sourceHash: "", entities: [], links: []},
    selected: null,
    found: [],
    problems: {entities: new Map(), links: new Map()},
    plan: null,
    backend: true,
    token: "",
};

const view = {x: 0, y: 0, k: 1};

const page = {
    canvas: document.getElementById("canvas"),
    viewport: document.getElementById("viewport"),
    links: document.getElementById("links"),
    nodes: document.getElementById("nodes"),
    ghost: document.getElementById("ghost"),
    palette: document.getElementById("palette"),
    findings: document.getElementById("findings"),
    inspector: document.getElementById("inspector"),
    project: document.getElementById("project"),
    verdict: document.getElementById("verdict"),
    hint: document.getElementById("hint"),
    review: document.getElementById("review"),
    apply: document.getElementById("apply"),
    sheet: document.getElementById("sheet"),
    sheetTitle: document.getElementById("sheet-title"),
    sheetGit: document.getElementById("sheet-git"),
    sheetFindings: document.getElementById("sheet-findings"),
    sheetDiff: document.getElementById("sheet-diff"),
    sheetClose: document.getElementById("sheet-close"),
};

// Talking to the server

class Refused extends Error {
    constructor(status, reason) {
        super(reason);
        this.status = status;
    }
}

async function request(method, path, body) {
    const headers = {"X-SynQt-Token": state.token};
    if (body !== undefined) {
        headers["Content-Type"] = "application/json";
    }
    let response = null;
    try {
        response = await fetch(path, {
            method,
            headers,
            body: body === undefined ? undefined : JSON.stringify(body),
        });
    } catch (error) {
        throw new Refused(0, String(error));
    }
    const text = await response.text();
    let payload = null;
    try {
        payload = text ? JSON.parse(text) : null;
    } catch (error) {
        payload = null;
    }
    if (!response.ok) {
        const reason = (payload && payload.error) || `${response.status} ${response.statusText}`;
        throw new Refused(response.status, reason);
    }
    return payload;
}

function tokenFromHash() {
    const hash = window.location.hash.replace(/^#/, "");
    return new URLSearchParams(hash).get("token") || "";
}

// Saying things

function say(message, level) {
    page.hint.textContent = message;
    page.hint.classList.toggle("stage__hint--error", level === "error");
}

function fail(error) {
    say(error && error.message ? error.message : String(error), "error");
}

function finding(item, onPick) {
    const row = document.createElement("li");
    row.className = `finding finding--${item.level}`;
    const rule = document.createElement("span");
    rule.className = "finding__rule";
    rule.textContent = item.rule;
    row.append(rule, document.createTextNode(item.message));
    if (onPick) {
        row.addEventListener("click", onPick);
    }
    return row;
}

function quiet(text) {
    const row = document.createElement("li");
    row.className = "finding finding--quiet";
    row.textContent = text;
    return row;
}

function renderFindings() {
    page.findings.replaceChildren();
    if (!state.found.length) {
        page.findings.append(quiet(state.design.entities.length
            ? "Nothing in the way. Review the changes when you are ready."
            : "An empty project. Add an entity to start."));
        return;
    }
    for (const item of state.found) {
        page.findings.append(finding(item, () => {
            select(item.link ? {kind: "link", name: item.link}
                             : {kind: "entity", name: item.entity});
        }));
    }
}

function renderVerdict() {
    const errors = state.found.filter((item) => item.level === "error").length;
    const warnings = state.found.length - errors;
    const parts = [];
    if (errors) {
        parts.push(`${errors} ${errors === 1 ? "problem" : "problems"}`);
    }
    if (warnings) {
        parts.push(`${warnings} to look at`);
    }
    page.verdict.textContent = parts.join(", ");
    page.verdict.className = "bar__verdict"
        + (errors ? " bar__verdict--error" : (warnings ? " bar__verdict--warn" : ""));
}

// Drawing

function applyView() {
    page.viewport.setAttribute("transform",
                               `translate(${view.x},${view.y}) scale(${view.k})`);
}

function validateLive() {
    state.found = ruleFindings(state.design);
    const entities = new Map();
    const links = new Map();
    for (const item of state.found) {
        for (const [map, key] of [[entities, item.entity], [links, item.link]]) {
            if (!key) {
                continue;
            }
            map.set(key, [...(map.get(key) || []), item]);
        }
    }
    state.problems = {entities, links};
}

function redraw() {
    validateLive();
    draw({links: page.links, nodes: page.nodes}, state.design,
         {problems: state.problems, selected: state.selected});
    renderFindings();
    renderVerdict();
}

function renderInspector() {
    inspect(page.inspector, state.design, state.selected, {
        changed: () => {
            touched();
            redraw();
        },
        rebuild: () => {
            touched();
            redraw();
            renderInspector();
        },
        rename: (kind, name) => {
            state.selected = {kind, name};
            touched();
            redraw();
        },
        removeEntity: (entity) => removeEntity(entity),
        removeLink: (link) => removeLink(link),
    });
}

// Any edit at all invalidates the change set that was last reviewed: Apply names a plan by
// its digest, and a document that has moved since is no longer the one that was shown.
function touched() {
    state.plan = null;
    page.apply.disabled = state.backend;
}

function select(what) {
    state.selected = what;
    redraw();
    renderInspector();
}

function fit() {
    const entities = state.design.entities || [];
    const box = page.canvas.getBoundingClientRect();
    if (!entities.length || !box.width) {
        view.x = 0;
        view.y = 0;
        view.k = 1;
        applyView();
        return;
    }
    const pad = 110;
    const left = Math.min(...entities.map((entity) => entity.x || 0)) - pad;
    const right = Math.max(...entities.map((entity) => entity.x || 0)) + pad;
    const top = Math.min(...entities.map((entity) => entity.y || 0)) - pad;
    const bottom = Math.max(...entities.map((entity) => entity.y || 0)) + pad;
    const scale = Math.min(box.width / (right - left), box.height / (bottom - top), 1.2);
    view.k = Math.min(Math.max(scale, ZOOM_RANGE[0]), ZOOM_RANGE[1]);
    view.x = ((box.width - ((right - left) * view.k)) / 2) - (left * view.k);
    view.y = ((box.height - ((bottom - top) * view.k)) / 2) - (top * view.k);
    applyView();
}

// The document

function adopt(design) {
    state.design = {
        version: design.version || 1,
        project: design.project || "app",
        sourceHash: design.sourceHash || "",
        entities: design.entities || [],
        links: design.links || [],
    };
    state.selected = null;
    page.project.textContent = state.design.project;
    document.title = `${state.design.project} - SynQt design`;
    touched();
    redraw();
    renderInspector();
}

function unique(base, taken) {
    if (!taken.has(base)) {
        return base;
    }
    let index = 2;
    while (taken.has(`${base}${index}`)) {
        index += 1;
    }
    return `${base}${index}`;
}

function column(role) {
    if (role === "client") {
        return COLUMNS.client;
    }
    return role === "edge" ? COLUMNS.edge : COLUMNS.service;
}

function place(role) {
    const wanted = column(role);
    const rows = (state.design.entities || [])
        .filter((entity) => column(roleOf(entity)) === wanted);
    return {
        x: wanted,
        y: rows.length ? Math.max(...rows.map((entity) => entity.y || 0)) + ROW_HEIGHT
                       : FIRST_Y,
    };
}

function addEntity(item) {
    const taken = new Set((state.design.entities || []).map((entity) => entity.name));
    const spot = place(item.role);
    const entity = {
        id: "",
        name: unique(item.base, taken),
        kind: "service",
        capability: "",
        blueprint: "",
        provider: "",
        targets: [],
        identity: false,
        ...item.make(),
        x: spot.x,
        y: spot.y,
    };
    entity.id = entity.name;
    state.design.entities.push(entity);
    touched();
    select({kind: "entity", name: entity.name});
    say(`Added '${entity.name}'. Drag the handle on its edge to another entity to connect `
        + "them.");
}

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}

function addLink(owner, consumer) {
    const taken = new Set((state.design.links || []).map((link) => link.name));
    const name = unique("link", taken);
    const link = {
        id: name,
        name,
        contract: capitalised(name),
        owner: owner.name,
        consumers: [consumer.name],
        instance: "shared",
        transport: "",
        members: [],
    };
    state.design.links.push(link);
    touched();
    select({kind: "link", name});
    say(`'${owner.name}' now owns '${name}' and '${consumer.name}' consumes it. Name it, `
        + "name its contract, and say what crosses it.");
}

function removeEntity(entity) {
    const name = entity.name;
    state.design.entities = state.design.entities.filter((one) => one !== entity);
    const orphaned = state.design.links.filter((link) => link.owner === name);
    state.design.links = state.design.links.filter((link) => link.owner !== name);
    for (const link of state.design.links) {
        link.consumers = (link.consumers || []).filter((consumer) => consumer !== name);
    }
    touched();
    select(null);
    say(orphaned.length
        ? `Removed '${name}', and with it the ${orphaned.length} connect point(s) it owned.`
        : `Removed '${name}'.`);
}

function removeLink(link) {
    state.design.links = state.design.links.filter((one) => one !== link);
    touched();
    select(null);
    say(`Removed '${link.name}'. The contract file it named is left where it is.`);
}

// The canvas, under the pointer

let drag = null;

function pointAt(event) {
    const box = page.canvas.getBoundingClientRect();
    const x = event.clientX - box.left;
    const y = event.clientY - box.top;
    return {screen: {x, y}, local: {x: (x - view.x) / view.k, y: (y - view.y) / view.k}};
}

function entityNamed(name) {
    return (state.design.entities || []).find((entity) => entity.name === name) || null;
}

function onDown(event) {
    if (event.button !== 0) {
        return;
    }
    const at = pointAt(event);
    const rim = event.target.closest("[data-rim]");
    const held = event.target.closest("[data-entity]");
    const link = event.target.closest("[data-link]");
    page.canvas.setPointerCapture(event.pointerId);

    if (rim) {
        drag = {mode: "link", from: entityNamed(rim.dataset.rim), at, moved: false};
        return;
    }
    if (held) {
        const entity = entityNamed(held.dataset.entity);
        drag = {
            mode: "entity",
            entity,
            offset: {x: at.local.x - (entity.x || 0), y: at.local.y - (entity.y || 0)},
            moved: false,
        };
        return;
    }
    if (link) {
        drag = {mode: "link-click", name: link.dataset.link, moved: false};
        return;
    }
    drag = {mode: "pan", at, from: {x: view.x, y: view.y}, moved: false};
    page.canvas.classList.add("is-panning");
}

function onMove(event) {
    if (!drag) {
        return;
    }
    const at = pointAt(event);
    if (drag.at) {
        const travelled = Math.hypot(at.screen.x - drag.at.screen.x,
                                     at.screen.y - drag.at.screen.y);
        drag.moved = drag.moved || travelled > DRAG_SLOP;
    }
    if (drag.mode === "entity") {
        drag.moved = true;
        drag.entity.x = Math.round(at.local.x - drag.offset.x);
        drag.entity.y = Math.round(at.local.y - drag.offset.y);
        touched();
        redraw();
        return;
    }
    if (drag.mode === "link" && drag.from) {
        page.ghost.replaceChildren(element("line", {
            class: "ghost",
            x1: drag.from.x || 0,
            y1: drag.from.y || 0,
            x2: at.local.x,
            y2: at.local.y,
        }));
        return;
    }
    if (drag.mode === "pan") {
        view.x = drag.from.x + (at.screen.x - drag.at.screen.x);
        view.y = drag.from.y + (at.screen.y - drag.at.screen.y);
        applyView();
    }
}

function onUp(event) {
    if (!drag) {
        return;
    }
    const finished = drag;
    drag = null;
    page.ghost.replaceChildren();
    page.canvas.classList.remove("is-panning");
    if (page.canvas.hasPointerCapture(event.pointerId)) {
        page.canvas.releasePointerCapture(event.pointerId);
    }

    if (finished.mode === "link" && finished.from) {
        const at = pointAt(event);
        const target = entityAt(state.design, at.local);
        if (!target || target === finished.from) {
            say("A connect point runs from the entity that owns it to one that consumes "
                + "it. Drop the line on the consumer.");
            return;
        }
        addLink(finished.from, target);
        return;
    }
    if (finished.mode === "entity") {
        select({kind: "entity", name: finished.entity.name});
        return;
    }
    if (finished.mode === "link-click") {
        select({kind: "link", name: finished.name});
        return;
    }
    if (finished.mode === "pan" && !finished.moved) {
        select(null);
    }
}

function onWheel(event) {
    event.preventDefault();
    const at = pointAt(event);
    const wanted = view.k * Math.exp(-event.deltaY * 0.0015);
    const next = Math.min(Math.max(wanted, ZOOM_RANGE[0]), ZOOM_RANGE[1]);
    view.x = at.screen.x - (at.local.x * next);
    view.y = at.screen.y - (at.local.y * next);
    view.k = next;
    applyView();
}

// The change set

function showSheet(title, git, found, body) {
    page.sheetTitle.textContent = title;
    page.sheetGit.textContent = git || "";
    page.sheetFindings.replaceChildren();
    for (const message of found || []) {
        const item = document.createElement("li");
        item.className = message.startsWith("error:") ? "finding finding--error"
                                                      : "finding finding--warn";
        item.textContent = message;
        page.sheetFindings.append(item);
    }
    page.sheetDiff.textContent = body;
    page.sheet.hidden = false;
}

async function review() {
    say("Working out what this would do...");
    try {
        const plan = await request("POST", "api/plan", {document: state.design});
        state.plan = plan;
        page.apply.disabled = !plan.ok;
        const count = plan.changes.length;
        showSheet(count ? `${count} file${count === 1 ? "" : "s"} would change`
                        : "Nothing to do: the project already says this",
                  plan.git, plan.findings,
                  plan.diff || "No file would change.");
        if (plan.stale) {
            say("synqt.yaml has changed on disk since this design was read. Reload the page "
                + "before applying anything.", "error");
            return;
        }
        say(plan.ok ? "Read it, then apply it."
                    : "This design does not pass `synqt check`, so it cannot be applied.",
            plan.ok ? "" : "error");
    } catch (error) {
        fail(error);
    }
}

async function applyPlan() {
    if (!state.backend) {
        download();
        return;
    }
    if (!state.plan) {
        say("Review the changes first: applying names the change set that was shown.",
            "error");
        return;
    }
    try {
        const answer = await request("POST", "api/apply",
                                     {document: state.design, digest: state.plan.digest});
        showSheet("Applied", "", answer.findings, answer.applied.join("\n"));
        adopt(answer.document);
        say(answer.ok ? "Applied. The project on disk is what you drew."
                      : "Applied, and `synqt check` still has something to say about it.",
            answer.ok ? "" : "error");
    } catch (error) {
        fail(error);
    }
}

function download() {
    const files = projectFiles(state.design);
    const blob = new Blob([zipBytes(files)], {type: "application/zip"});
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${state.design.project || "app"}.zip`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 10000);
    showSheet(`${files.length} file${files.length === 1 ? "" : "s"} downloaded`, "", [],
              files.map((file) => `# ${file.name}\n\n${file.text}`).join("\n"));
    say("Unzip it over a project made with `synqt new`, or run `synqt design` in one and "
        + "edit it in place.");
}

// Starting up

function buildPalette() {
    for (const item of PALETTE) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "palette__item";
        const swatch = document.createElement("span");
        swatch.className = `palette__swatch palette__swatch--${item.role}`;
        button.append(swatch, document.createTextNode(item.label));
        button.addEventListener("click", () => addEntity(item));
        page.palette.append(button);
    }
}

function goOffline(reason) {
    state.backend = false;
    page.review.hidden = true;
    page.apply.textContent = "Download";
    page.apply.disabled = false;
    adopt({version: 1, project: "app", entities: [], links: []});
    say(reason);
}

async function load() {
    state.token = tokenFromHash();
    try {
        const answer = await request("GET", "api/project");
        state.backend = true;
        adopt(answer.document);
        fit();
        say(answer.ok ? "Editing this project. Nothing is written until you apply a change "
                      + "set you have read."
                      : "`synqt check` already has something to say about this project.",
            answer.ok ? "" : "error");
    } catch (error) {
        if (error.status === 403) {
            page.review.disabled = true;
            page.apply.disabled = true;
            say(`${error.message}`, "error");
            return;
        }
        goOffline("No SynQt on the other end of this page, so this is a drawing board: "
                  + "design a project here and download it, or run `synqt design` in a "
                  + "project to edit that one in place.");
    }
}

function wire() {
    page.canvas.addEventListener("pointerdown", onDown);
    page.canvas.addEventListener("pointermove", onMove);
    page.canvas.addEventListener("pointerup", onUp);
    page.canvas.addEventListener("pointercancel", onUp);
    page.canvas.addEventListener("wheel", onWheel, {passive: false});
    page.review.addEventListener("click", () => review());
    page.apply.addEventListener("click", () => applyPlan());
    page.sheetClose.addEventListener("click", () => {
        page.sheet.hidden = true;
    });
    window.addEventListener("resize", () => fit());
}

buildPalette();
wire();
renderInspector();
load();
