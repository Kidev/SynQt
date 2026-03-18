// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The card that opens over whatever the pointer is on.
//
// It is a function of the design document and nothing else: what is under the pointer, read
// off the drawing's own data attributes, and the document that drawing was made from. Which
// is why it is here rather than in the editor -- the home page draws the same picture with
// the same `draw`, and a second, thinner tooltip written for it would be a second answer to
// what an entity is. The editor hands it the findings it has and the palette rows it draws;
// a page with neither passes neither, and every card that does not depend on them is the
// same card.
//
// Nothing here reads the editor's state, opens anything, or writes anywhere: it builds a
// detached element and hands it back. Where it is put and how it is placed belong to
// whoever asked.

import { frontsOf, gatesOf, runsSignIn } from "./rules.js";
import { MEMBER_KINDS, ROLE_HELP, aboveTheScope, accessorName, describe, endsOfPoint,
         glyphSvg, linkTitleNode, memberCode, memberMarkSvg, memberParts, roleOf, scopeGate,
         seatsOfFront } from "./canvas.js";
import { entityFiles, isShared } from "./project.js";

// A page with no checker behind it has no findings, which is not the same as having none to
// show. The two maps are what the card reads, so it is handed empty ones rather than asked
// to test for them.
const NO_PROBLEMS = {entities: new Map(), links: new Map()};

function entityNamed(design, name) {
    return (design.entities || []).find((one) => one.name === name) || null;
}

function tipRow(label, value) {
    const row = document.createElement("div");
    row.className = "tip__row";
    const name = document.createElement("span");
    name.className = "tip__label";
    name.textContent = label;
    const said = document.createElement("span");
    said.className = "tip__value";
    said.textContent = value;
    row.append(name, said);
    return row;
}

// A rule across the tip with a word on it, so the parts of a card are parts and not one column
// of lines that happen to be in an order. Ten rows in one grey face
// are a paragraph with the punctuation taken out.
function tipSection(label) {
    const row = document.createElement("div");
    row.className = "tip__section";
    row.textContent = label;
    return row;
}

// One end of a link, named and coloured as the role it plays. The same two colours the drawing
// puts on the entities themselves at the same moment, so the word here and the disc out there
// are one statement. This one owns it, that one consumes it.
function tipParty(design, role, names) {
    const row = document.createElement("div");
    row.className = `tip__party tip__party--${role}`;
    const label = document.createElement("span");
    label.className = "tip__label";
    label.textContent = role;
    row.append(label);
    const held = document.createElement("span");
    held.className = "tip__names";
    if (!names.length) {
        held.append(quietName(role === "owner" ? "nobody" : "nobody yet"));
    }
    for (const name of names) {
        const entity = entityNamed(design, name);
        const chip = document.createElement("span");
        chip.className = `tip__chip${entity ? ` tip__chip--${roleOf(entity)}` : ""}`;
        if (entity) {
            chip.append(glyphSvg(roleOf(entity)));
        }
        chip.append(document.createTextNode(name));
        held.append(chip);
    }
    row.append(held);
    return row;
}

function quietName(text) {
    const said = document.createElement("span");
    said.className = "tip__chip tip__chip--none";
    said.textContent = text;
    return said;
}

// One member of a contract, painted the way the canvas paints it. The mark that says which of
// the four kinds it is, then the declaration in the same three syntax colours the file pane
// uses. A list of these read as one grey block before, which is the one part of a link tip
// somebody is following the line to find.
function tipMember(member, link) {
    const row = document.createElement("div");
    row.className = `tip__member${aboveTheScope(member, link) ? " is-scoped" : ""}`;
    row.append(memberMarkSvg(member.kind));
    const code = document.createElement("span");
    code.className = "tip__code";
    for (const part of memberParts(member)) {
        const run = document.createElement("span");
        run.className = `tip__tok tip__tok--${part.kind}`;
        run.textContent = part.text;
        code.append(run);
    }
    const gate = scopeGate(member, link);
    if (gate) {
        const run = document.createElement("span");
        run.className = "tip__tok tip__tok--scope";
        run.textContent = ` ${gate}`;
        code.append(run);
    }
    row.append(code);
    return row;
}

// What one member carries, written out in full. The canvas has room for the types alone, so
// this is where the names that go with them live. A row's roles are what a consumer's delegate
// reads by name, and a call's parameters are what somebody writing the call has to supply, so
// neither is decoration.
function partsRow(member) {
    if (member.kind === "prop") {
        return tipRow("holds", `One ${member.type || "var"}`);
    }
    if (member.kind === "model") {
        return (member.roles || []).length
            ? tipRow("rows carry", (member.roles || [])
                .map((role) => `${role.type} ${role.name}`).join(", "))
            : tipRow("rows carry", "No roles yet, so no part of a row crosses");
    }
    const params = member.params || [];
    return tipRow("takes", params.length
        ? params.map((part) => `${part.type} ${part.name}`).join(", ")
        : "Nothing");
}

export function tipFor(design, what, {problems = NO_PROBLEMS, palette = []} = {}) {
    const box = document.createElement("div");
    // One member of one contract, asked for by pointing anywhere on its row: the mark that
    // says which of the four kinds it is, or the name and prototype beside it. Both, because
    // pointing at `placeBid` is the obvious way to ask what `placeBid` is, and for as long as
    // only the mark answered, the obvious way did nothing.
    //
    // The canvas writes the row short. Types without their names, and no word for the kind.
    // Everything it left out is here, said about the two entities at the ends of
    // this link rather than about owners and consumers in general.
    if (what.kind === "member") {
        const link = (design.links || []).find((one) => one.name === what.link);
        const member = ((link || {}).members || [])
            .find((one) => one.name === what.name);
        if (!member) {
            return null;
        }
        const said = MEMBER_KINDS[member.kind] || MEMBER_KINDS.prop;
        const ends = endsOfPoint(link);
        const head = document.createElement("div");
        head.className = "tip__head tip__head--link";
        head.append(memberMarkSvg(member.kind));
        // The member as the line of code it is, in the same runs the file pane and the row on
        // the canvas are painted in. It was one grey string, which is a card titled with the
        // one thing on it a reader can already see spelled out below.
        head.append(memberCode(member));
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = said.name;
        head.append(kind);
        box.append(head);
        // The two ends of this particular member's journey, in the same two colours the
        // drawing puts on the entities while it is hovered.
        box.append(tipParty(design, "owner", link.owner ? [link.owner] : []));
        box.append(tipParty(design, "consumer", link.consumers || []));
        box.append(tipSection("what it carries"));
        box.append(partsRow(member));
        if (member.kind === "slot") {
            box.append(tipRow("answers", member.type
                ? `${member.type}, so the call resolves with a value`
                : "Nothing, so the call is made and not waited on"));
        }
        // What reaches this member is its own gate, or the point's where it names none. The
        // second half is the ordinary case and is not news. A member that only inherits the
        // point's scope is not "held back" from anything, and saying so of every member of a
        // gated point is saying it of nothing.
        const raised = aboveTheScope(member, link);
        const gate = member.scope || link.scope;
        box.append(tipRow("reaches", raised
            ? `Callers holding '${member.scope}', and nobody else`
            : (gate
                ? `Callers holding '${gate}', the connect point's own gate`
                : "Any caller, anonymous included")));
        box.append(tipHelp(said.says(ends.owner, ends.consumers)));
        if (raised) {
            box.append(tipHelp(`Raised above ${link.scope ? `'${link.scope}'`
                                                          : "the connect point's own scope"}, `
                               + "so this member alone is held back from callers the rest of "
                               + `'${link.owner}' answers.`));
        }
        return box;
    }
    // One scope on a front's back: who answers callers holding it, and what reaching them
    // costs a browser. The seat is the whole of the routing on the canvas, so the one thing it
    // cannot say in a word -- what happens to a caller of this scope -- is said here.
    if (what.kind === "seat") {
        const front = frontsOf(design).get(what.name);
        const seat = (seatsOfFront(front) || []).find((one) => one.scope === what.scope);
        if (!seat) {
            return null;
        }
        const head = document.createElement("div");
        head.className = "tip__head tip__head--edge";
        const title = document.createElement("span");
        title.textContent = what.scope;
        head.append(title);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = "scope";
        head.append(kind);
        box.append(head);
        box.append(tipRow("on", `'${what.name}', which fronts for the entities behind it`));
        if (seat.tier) {
            box.append(tipParty(design, "owner", [seat.tier]));
            box.append(tipParty(design, "consumer", [what.name]));
        } else {
            box.append(tipRow("answered by",
                              "Nobody yet, so a caller of this scope is handed nowhere"));
        }
        box.append(tipHelp(seat.tier
            ? `A browser holding '${what.scope}' reaches '${what.name}' and is served by `
              + `'${seat.tier}'. It never learns that '${seat.tier}' exists: the accessor it `
              + `writes is ${accessorName(what.name)}, whoever is behind it.`
            : `Drag from here to the entity that serves callers holding '${what.scope}', or `
              + "from that entity to here. Either way round draws the same routing."));
        return box;
    }
    // A box around a group of entities. Its name is on the canvas and what it means is here,
    // because the meaning is the same three sentences on every glance and the arrangement is
    // what somebody is looking at.
    if (what.kind === "zone") {
        const head = document.createElement("div");
        head.className = "tip__head tip__head--link";
        const title = document.createElement("span");
        title.textContent = what.name;
        head.append(title);
        box.append(head);
        box.append(tipHelp(what.note));
        return box;
    }
    // The break on a line that reaches a front nothing routes to. What has stopped being true
    // about it, and the one gesture that puts it back.
    if (what.kind === "break") {
        const link = (design.links || []).find((one) => one.name === what.name);
        if (!link) {
            return null;
        }
        const head = document.createElement("div");
        head.className = "tip__head tip__head--broken";
        head.append(linkTitleNode(link, what.consumer));
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = "broken";
        head.append(kind);
        box.append(head);
        box.append(tipParty(design, "owner", link.owner ? [link.owner] : []));
        box.append(tipParty(design, "consumer", what.consumer ? [what.consumer] : []));
        box.append(tipHelp(`'${what.consumer}' hands its callers to the entities behind it, `
                           + `and no scope is handed to '${link.owner}'. The link is still `
                           + `there and nothing travels down it: nobody is ever routed to `
                           + `this end of it.`));
        box.append(tipHelp("Drag from the cross onto a scope on the front's back to say whose "
                           + "callers it serves. Press it to select the line."));
        return box;
    }
    // A row in the rail is the entity it would add, so it says what the node on the canvas
    // says, in the same box. A `title` attribute said the same words in the browser's own
    // tooltip, which arrives a second late and looks like it belongs to a different program.
    if (what.kind === "role") {
        const item = palette.find((one) => one.role === what.name);
        if (!item) {
            return null;
        }
        const head = document.createElement("div");
        head.className = `tip__head tip__head--${item.role}`;
        head.append(glyphSvg(item.role));
        const title = document.createElement("span");
        title.textContent = item.label;
        head.append(title);
        box.append(head);
        box.append(tipHelp(item.help));
        return box;
    }
    if (what.kind === "entity") {
        const entity = entityNamed(design, what.name);
        if (!entity) {
            return null;
        }
        const role = roleOf(entity);
        // A gate keeps a client's colour and takes the barrier's glyph, exactly as the node
        // on the canvas does, so the card that opens over it is the thing that was pointed at.
        const gate = gatesOf(design).get(entity.name) || "";
        const head = document.createElement("div");
        head.className = `tip__head tip__head--${role}`;
        head.append(glyphSvg(gate ? "gate" : role));
        const title = document.createElement("span");
        title.textContent = entity.name;
        head.append(title);
        box.append(head);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        // `web_edge` is what the configuration writes; `web edge` is what a card set in small
        // capitals should read as.
        kind.textContent = describe(entity).replace(/_/g, " ");
        head.append(kind);
        if (gate) {
            box.append(tipRow("the gate", `What '${gate}' serves a session that has signed `
                                          + "in as nobody. A signed-in session is served a "
                                          + "different bundle, and cannot fetch a file of "
                                          + "this one."));
        }
        box.append(tipRow("reachable from",
                          role === "client" ? "The person using it"
                          : (role === "edge" ? "The internet, and only over TLS"
                                             : "The entities on its consumer lists, and "
                                               + "nothing else")));
        // The mark the canvas draws on this node, said in words. It is the switch a whole
        // project hangs off: without it nobody ever leaves the default scope, so every
        // member gate refuses everybody and every bundle above the first is unreachable.
        if (runsSignIn(entity)) {
            box.append(tipRow("signs people in",
                              "It runs the OAuth exchange, keeps the tokens and the "
                              + "sessions, and hands the browser a cookie. This is what "
                              + "makes Session.login() in a client reach anything."));
        }
        // How many of it there are, which is the entity's own answer and decides whether a
        // Source holds one caller's state or everybody's. It is a setting on this node, so it
        // is a fact about this node.
        if (role !== "client") {
            box.append(tipRow("how many", isShared(entity)
                ? "One, for everybody, and every caller still arrives with a Caller of "
                  + "their own"
                : "One per caller, holding only what is theirs"));
        }
        // What this entity is at either end of, said in the same two role colours a hovered
        // link paints its ends with. Which end
        // of a link an entity is on is the one question about it that a list answers and a
        // comma-joined sentence does not.
        const owns = (design.links || [])
            .filter((link) => link.owner === entity.name);
        const uses = (design.links || [])
            .filter((link) => (link.consumers || []).includes(entity.name));
        if (owns.length || uses.length) {
            box.append(tipSection("on the mesh"));
        }
        for (const link of owns) {
            box.append(tipParty(design, "owner", [entity.name]));
            box.append(tipParty(design, "consumer", link.consumers || []));
        }
        if (uses.length) {
            box.append(tipRow("consumes", uses.map((link) => link.owner).join(", ")));
        }
        if (!owns.length && !uses.length) {
            box.append(tipRow("on the mesh", "Nothing reaches it and it reaches nothing"));
        }
        const front = frontsOf(design).get(entity.name);
        if (front) {
            const wired = seatsOfFront(front).filter((seat) => seat.tier);
            box.append(tipRow("hands on", wired.length
                ? wired.map((seat) => `${seat.scope} to '${seat.tier}'`).join(", ")
                : "Nothing yet, so it hands nobody anywhere"));
        }
        const files = entityFiles(design, entity);
        box.append(tipRow("files", files.length
            ? files.map((file) => file.name).join(", ") : "None yet"));
        box.append(tipHelp(ROLE_HELP[role]));
        box.append(...tipFindings(problems.entities.get(entity.name) || []));
        return box;
    }
    const link = (design.links || []).find((one) => one.name === what.name);
    if (!link) {
        return null;
    }
    const head = document.createElement("div");
    head.className = "tip__head tip__head--link";
    head.append(linkTitleNode(link, what.consumer));
    const kind = document.createElement("span");
    kind.className = "tip__kind";
    kind.textContent = "connect point";
    head.append(kind);
    box.append(head);

    // The two ends first, in the two role colours, because which entity decides and which one
    // only asks is what somebody follows a line to find out. As two of eight grey
    // rows, each carrying a clause of explanation, the pair is harder to pick out rather
    // than easier.
    box.append(tipParty(design, "owner", link.owner ? [link.owner] : []));
    // One line is one consumer of a contract they all share, so a hovered line names that one
    // and says how many others there are. The icon names all of them.
    const consumers = link.consumers || [];
    box.append(tipParty(design, "consumer", what.consumer ? [what.consumer] : consumers));
    if (what.consumer && consumers.length > 1) {
        box.append(tipHelp(`One of ${consumers.length} consuming it. Every one of them gets `
                           + "the same contract."));
    }

    box.append(tipSection("how"));
    box.append(tipRow("written as", link.owner
        ? `${accessorName(link.owner)}, in every consumer's QML`
        : "Nothing yet: the owner is the name"));
    box.append(tipRow("carried over", link.transport === "local"
        ? "A local socket, so the caller is trusted by colocation"
        : "Mutual TLS, verified against the project CA"));
    box.append(tipRow("gated behind", link.scope
        ? `'${link.scope}', and a browser below it never acquires it`
        : "Nothing, so any session reaches it"));
    const behind = seatsOfFront(frontsOf(design).get(link.owner))
        .filter((seat) => seat.tier);
    if (behind.length) {
        box.append(tipRow("handed on to",
                          behind.map((seat) => `${seat.scope} to '${seat.tier}'`).join(", ")));
    }

    const members = link.members || [];
    if (members.length) {
        box.append(tipSection(`crosses (${members.length})`));
        const list = document.createElement("div");
        list.className = "tip__members";
        for (const member of members) {
            list.append(tipMember(member, link));
        }
        box.append(list);
    } else {
        box.append(tipHelp(link.owner
            ? `Nothing crosses it yet. Tick what '${link.owner}' declares onto the contract, `
              + "and nothing undeclared will ever cross whatever anyone writes."
            : "Nothing crosses it yet. Nothing undeclared ever will."));
    }
    box.append(...tipFindings(problems.links.get(link.name) || []));
    return box;
}

function tipHelp(text) {
    const note = document.createElement("p");
    note.className = "tip__help";
    note.textContent = text;
    return note;
}

function tipFindings(found) {
    return found.map((item) => {
        const row = document.createElement("p");
        row.className = `tip__finding tip__finding--${item.level}`;
        row.textContent = item.message;
        return row;
    });
}

// What the pointer is on, as the card's own question.
//
// Read off the drawing rather than off the document: every part of it carries the name of
// what it is, so this is one `closest` per kind, in the order of what is the smaller thing
// under the pointer. The order is the whole of it -- a member's row is on a line, and a line
// is in a box.
export function whatIsUnder(target) {
    if (!target || !target.closest) {
        return null;
    }
    // One member of a contract, before the link it is written beside: the row is the smaller
    // thing under the pointer, and it is answered by anywhere on it -- the mark that says
    // which of the four kinds it is, or the name and prototype next to it.
    const member = target.closest("[data-member]");
    if (member) {
        const holder = member.closest("[data-link]");
        // Which line the row is written beside, as well as which point it belongs to: the
        // block is drawn once per consumer, and lighting the point without the line left the
        // one line the pointer was on unlit.
        return {kind: "member", name: member.dataset.member,
                link: holder ? holder.dataset.link : "",
                consumer: holder ? (holder.dataset.consumer || "") : ""};
    }
    // The break on a line that reaches a front nothing routes to. Before the link it sits on,
    // because it is the smaller thing under the pointer and it answers a different question:
    // the line says what crosses, and this says why none of it arrives.
    const broke = target.closest("[data-break]");
    if (broke) {
        return {kind: "break", name: broke.dataset.break,
                consumer: broke.dataset.breakConsumer || ""};
    }
    // A scope on a front's back, before the node it is drawn in. It is the seat, and what it
    // says is where callers of that scope go.
    const seat = target.closest("[data-seat]");
    if (seat) {
        return {kind: "seat", name: seat.dataset.seat, scope: seat.dataset.scope};
    }
    const entity = target.closest("[data-entity]");
    if (entity) {
        return {kind: "entity", name: entity.dataset.entity};
    }
    // The contract icon before the lines, because it is drawn over them and is the whole
    // point rather than one consumer of it.
    const contract = target.closest("[data-contract]");
    if (contract) {
        return {kind: "contract", name: contract.dataset.contract};
    }
    const link = target.closest("[data-link]");
    if (link) {
        // A broken line is the break, wherever on it the pointer is. The cross is one mark
        // near the far end and the line is what a reader's pointer finds first: answering the
        // line with the ordinary card would have it describe a link as though it worked.
        if (link.dataset.broken) {
            return {kind: "break", name: link.dataset.link,
                    consumer: link.dataset.consumer || ""};
        }
        return {kind: "link", name: link.dataset.link, consumer: link.dataset.consumer || ""};
    }
    // A box's name, last, because everything drawn inside a box answers for itself first.
    // What the box means is written on the name rather than under it, so this is where it is
    // read from.
    const zone = target.closest("[data-zone-title]");
    return zone
        ? {kind: "zone", name: zone.textContent, note: zone.dataset.note || ""}
        : null;
}
