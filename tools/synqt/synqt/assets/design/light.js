// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What lights up on the drawing, and why.
//
// Here rather than inside the editor because the editor is not the only thing that draws
// this picture. The home page draws it too, with the same `draw`, out of the same document.
// A second, thinner answer to "which end of this line is that entity" written for that page
// would be a second answer, and the first week either changed they would disagree. Same
// reasoning that put the card in tip.js.
//
// Two states, two colours. Selection is what the panel has open. It stays where
// it was put and is what a reader is working on. Hover is where the pointer is this instant.
// In one colour the drawing lost track of the first every time somebody moved the pointer
// across it, which on a canvas of a dozen links is exactly when knowing which line you are
// working on matters most.
//
// A thing hovered lights everything that is the same fact as itself. A line lights the
// contract it carries, because that is what crosses it, and the scope seat it lands on,
// because that is who answers it. Pointing at a line and being shown only the line leaves
// the two ends of the question (what crosses, and who serves it) for the reader to trace
// by eye across whatever else the canvas holds.
//
// Nothing here reads the editor's state and nothing here redraws. `redraw()` from a
// pointermove path is how the double-click bug comes back. All of this puts a class on an
// element already in the document and takes it off again.

import { frontsOf } from "./rules.js";
import { seatsOfFront } from "./canvas.js";

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

// Everything one hovered thing lights, as the keys the drawing's elements are found by.
export function hoverSet(design, what) {
    // `owners` and `consumers` are the two ends of whatever is hovered, kept apart because
    // which of the two an entity is, is the first thing anybody wants off a line, and the
    // drawing said it only in the direction of an arrowhead. Lit in the two role colours, the
    // same two the tip uses for the words, so the picture and the words say it together.
    const empty = {points: new Set(), lines: new Set(), seats: new Set(),
                   entities: new Set(), zones: new Set(), members: new Set(),
                   owners: new Set(), consumers: new Set()};
    if (!what) {
        return empty;
    }
    const named = (name) => (design.links || []).find((one) => one.name === name);
    if (what.kind === "entity") {
        empty.entities.add(what.name);
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
        // The pair only, and only when there is a pair. A seat nothing is wired to has no
        // owner to be the other half of, and marking the front alone put the word CONSUMER
        // over an entity with nothing on the far end of it.
        if (behind) {
            empty.points.add(behind.name);
            empty.lines.add(`${behind.name}\n${what.name}`);
            empty.owners.add(behind.name);
            empty.consumers.add(what.name);
        }
        return empty;
    }
    // The break lights the line it is on and every seat it could be dropped on, because the
    // seats are where the fix is and a reader looking at the break is looking for it.
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
// not when the pointer is on some other point. The selection has already written OWNER and
// CONSUMER on its own two ends and left them there, so a second pair from the pointer lands
// on a chain (a > b > c, with (a > b) selected and (b > c) under the pointer), and the
// entity in the middle carries both words at once, saying it is the owner and the consumer
// of nothing in particular. While something is selected, hovering elsewhere says where the
// pointer is and no more.
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
            ".is-hover, .is-hovered, .is-owner, .is-consumer")) {
        marked.classList.remove("is-hover");
        marked.classList.remove("is-hovered");
        marked.classList.remove("is-owner");
        marked.classList.remove("is-consumer");
    }
}

// What is selected keeps saying the two things hovering it says: which entity owns the point
// and which ones consume it, and which way round the line runs.
//
// A selection is what somebody is working on, and it is the state they are in while they
// read the panel beside it, add a member to it or change who gets it. Saying "owner" and
// "consumer" only under the pointer meant the two ends of the thing in hand went dark the
// moment the pointer left the line to reach the panel, which is every time. Its own classes
// rather than the hover ones, because clearHighlight() takes those off whenever the pointer
// leaves the canvas, which, again, is what reaching for the panel is.
export function litSelection(root, design, selected) {
    const wanted = hoverSet(design, selected);
    for (const node of root.querySelectorAll("[data-entity]")) {
        node.classList.toggle("is-lit-owner", wanted.owners.has(node.dataset.entity));
        node.classList.toggle("is-lit-consumer", wanted.consumers.has(node.dataset.entity));
    }
}
