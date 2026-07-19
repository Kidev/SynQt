// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What lights up on the drawing, and why.
//
// Here and not inside the editor, because the home page draws this picture too, with the
// same `draw` and the same document, and a second copy for that page would drift. tip.js
// holds the card for the same reason.
//
// Two states, two colours. Selection is what the panel has open, and it stays put. Hover is
// where the pointer is now. In one colour, moving the pointer across a dozen links would lose
// the line being worked on.
//
// A hovered thing lights everything that states the same fact. A line lights the contract it
// carries (what crosses it) and the scope seat it lands on (who answers it).
//
// Nothing here reads the editor's state and nothing here redraws: a `redraw()` from a
// pointermove path loses double-click. All of this puts a class on an element already in
// the document and takes it off again.

import { frontsOf, runsSignIn } from "./rules.js";
import { roleOf, seatsOfFront } from "./canvas.js";

// Which scope seats a link arrives at. On a front, the seat of the scope whose callers this
// link's owner serves. `entity\nscope`, the same key the seat elements are found by.
function seatsOfLink(design, link) {
    const fronts = frontsOf(design);
    const found = [];
    for (const consumer of link.consumers || []) {
        const front = fronts.get(consumer);
        if (!front) {
            continue;
        }
        for (const seat of seatsOfFront(front)) {
            if (seat.tier === link.owner) {
                found.push(`${consumer}\n${seat.scope}`);
            }
        }
    }
    return found;
}

// The lines from an edge that signs people in to a browser, as line keys. These are the lines
// whose sign-in hint shows (canvas.js signInHint) while the edge, its mark or the line itself
// is hovered.
function signInLines(design, edgeName, only) {
    const entities = design.entities || [];
    const edge = entities.find((one) => one.name === edgeName);
    if (!edge || !runsSignIn(edge)) {
        return [];
    }
    const found = [];
    for (const link of design.links || []) {
        if (link.owner !== edgeName) {
            continue;
        }
        for (const consumer of link.consumers || []) {
            const entity = entities.find((one) => one.name === consumer);
            if (entity && roleOf(entity) === "client" && (!only || only === consumer)) {
                found.push(`${link.name}\n${consumer}`);
            }
        }
    }
    return found;
}

// Everything one hovered thing lights, as the keys the drawing's elements are found by.
export function hoverSet(design, what) {
    // `owners` and `consumers` are the two ends of whatever is hovered, kept apart so each is
    // lit in its role colour, the same two the tip uses for the words.
    //
    // `signins` is the lines whose sign-in hint shows. Pointing at the edge or at its mark
    // shows it on every line to a browser, and pointing at one such line shows it on that one.
    const empty = {points: new Set(), lines: new Set(), seats: new Set(),
                   entities: new Set(), zones: new Set(), members: new Set(),
                   owners: new Set(), consumers: new Set(), signins: new Set()};
    if (!what) {
        return empty;
    }
    const named = (name) => (design.links || []).find((one) => one.name === name);
    if (what.kind === "entity" || what.kind === "signin") {
        empty.entities.add(what.name);
        for (const key of signInLines(design, what.name)) {
            empty.signins.add(key);
        }
        return empty;
    }
    if (what.kind === "zone") {
        empty.zones.add(what.name);
        return empty;
    }
    // A seat, and the link that lands on it. The entity behind a scope reaches the front
    // through the point it owns, so that point is the other half of what the seat says.
    if (what.kind === "seat") {
        empty.seats.add(`${what.name}\n${what.scope}`);
        const front = frontsOf(design).get(what.name);
        const seat = (seatsOfFront(front) || []).find((one) => one.scope === what.scope);
        const behind = seat && seat.tier ? named(seat.tier) : null;
        // The pair only, and only when there is a pair: a seat nothing is wired to has no
        // owner, and the front alone is not a consumer of anything.
        if (behind) {
            empty.points.add(behind.name);
            empty.lines.add(`${behind.name}\n${what.name}`);
            empty.owners.add(behind.name);
            empty.consumers.add(what.name);
        }
        return empty;
    }
    // A break lights the line it is on and every seat it could be dropped on, which is where
    // the fix is.
    if (what.kind === "break") {
        const link = named(what.name);
        if (!link) {
            return empty;
        }
        empty.points.add(link.name);
        empty.lines.add(`${link.name}\n${what.consumer || ""}`);
        empty.owners.add(String(link.owner || ""));
        const front = frontsOf(design).get(what.consumer);
        for (const seat of seatsOfFront(front)) {
            empty.seats.add(`${what.consumer}\n${seat.scope}`);
        }
        return empty;
    }
    const link = named(what.kind === "member" ? what.link : what.name);
    if (!link) {
        return empty;
    }
    empty.points.add(link.name);
    for (const key of seatsOfLink(design, link)) {
        empty.seats.add(key);
    }
    if (what.kind === "member") {
        empty.members.add(`${link.name}\n${what.name}`);
    }
    // A contract is the whole point, so every line out of it lights. One line is one consumer
    // of it, so only that line does.
    if (what.kind === "contract") {
        for (const consumer of link.consumers || []) {
            empty.lines.add(`${link.name}\n${consumer}`);
            empty.consumers.add(consumer);
        }
        empty.lines.add(`${link.name}\n`);
    } else {
        empty.lines.add(`${link.name}\n${what.consumer || ""}`);
        // One line is one consumer, so only that one lights. The icon and a member row belong
        // to the whole point, so all of them do.
        for (const consumer of (what.consumer ? [what.consumer] : (link.consumers || []))) {
            empty.consumers.add(consumer);
        }
    }
    if (link.owner) {
        empty.owners.add(link.owner);
        if (what.kind === "link") {
            for (const key of signInLines(design, link.owner, what.consumer || "")) {
                empty.signins.add(key);
            }
        }
    }
    return empty;
}


export function hoverKey(what) {
    if (!what) {
        return "";
    }
    return [what.kind, what.name, what.consumer || "", what.link || "",
            what.scope || ""].join("\n");
}

// Whether what the pointer is on may say which end of a link an entity is, given what is
// selected. `wanted` is that thing's own `hoverSet`, already worked out by the caller.
//
// It may when nothing is selected, and when the pointer is on the selection itself. It may
// not when the pointer is on another point: the selection already shows OWNER and CONSUMER
// on its two ends, and on a chain (a > b > c, with (a > b) selected and (b > c) under the
// pointer) a second pair would put both words on b at once.
function roleFromHover(design, wanted, selected) {
    if (!selected) {
        return true;
    }
    const chosen = hoverSet(design, selected);
    for (const point of wanted.points) {
        if (chosen.points.has(point)) {
            return true;
        }
    }
    return false;
}

// Light everything `what` lights, inside `root`. `selected` is what the panel has open, or
// null on a drawing with no panel behind it.
export function highlight(root, design, what, selected) {
    const wanted = hoverSet(design, what);
    const roles = roleFromHover(design, wanted, selected);
    for (const group of root.querySelectorAll("[data-link]")) {
        const line = `${group.dataset.link}\n${group.dataset.consumer || ""}`;
        group.classList.toggle("is-hover", wanted.lines.has(line));
        group.classList.toggle("is-signin", wanted.signins.has(line));
    }
    for (const badge of root.querySelectorAll("[data-contract]")) {
        badge.classList.toggle("is-hover", wanted.points.has(badge.dataset.contract));
    }
    for (const row of root.querySelectorAll("[data-member]")) {
        const holder = row.closest("[data-link]");
        row.classList.toggle("is-hover", wanted.members.has(
            `${holder ? holder.dataset.link : ""}\n${row.dataset.member}`));
    }
    for (const grab of root.querySelectorAll("[data-seat]")) {
        grab.classList.toggle("is-hovered",
                              wanted.seats.has(`${grab.dataset.seat}\n${grab.dataset.scope}`));
    }
    for (const node of root.querySelectorAll("[data-entity]")) {
        node.classList.toggle("is-hover", wanted.entities.has(node.dataset.entity));
        node.classList.toggle("is-owner",
                              roles && wanted.owners.has(node.dataset.entity));
        node.classList.toggle("is-consumer",
                              roles && wanted.consumers.has(node.dataset.entity));
    }
    for (const box of root.querySelectorAll("[data-zone-title]")) {
        const zone = box.closest(".zone");
        if (zone) {
            zone.classList.toggle("is-hover", wanted.zones.has(box.dataset.zoneTitle));
        }
    }
}

// Whatever was lit, unlit. Called when the pointer leaves the drawing and before a redraw,
// so nothing is left glowing under a pointer that has gone.
export function clearHighlight(root) {
    for (const marked of root.querySelectorAll(
            ".is-hover, .is-hovered, .is-owner, .is-consumer, .is-signin")) {
        marked.classList.remove("is-hover");
        marked.classList.remove("is-hovered");
        marked.classList.remove("is-owner");
        marked.classList.remove("is-consumer");
        marked.classList.remove("is-signin");
    }
}

// What is selected keeps saying the two things hovering it says: which entity owns the point
// and which ones consume it, and which way round the line runs.
//
// The selection is what someone works on while the pointer is in the panel, so its two ends
// stay marked after the pointer leaves the line. It has its own classes because
// clearHighlight() takes the hover ones off whenever the pointer leaves the canvas.
export function litSelection(root, design, selected) {
    const wanted = hoverSet(design, selected);
    for (const node of root.querySelectorAll("[data-entity]")) {
        node.classList.toggle("is-lit-owner", wanted.owners.has(node.dataset.entity));
        node.classList.toggle("is-lit-consumer", wanted.consumers.has(node.dataset.entity));
    }
}
