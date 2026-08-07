// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The card that opens over whatever the pointer is on.
//
// A function of what is under the pointer (read off the drawing's data attributes) and the
// design document, and nothing else, so the home page, which draws the same picture with the
// same `draw`, shows the same cards. The editor also passes its findings and palette rows; a
// page without them passes neither.
//
// Nothing here reads the editor's state or writes anywhere: it builds a detached element and
// returns it, and the caller places it.

import { frontsOf, gatesOf, runsSignIn } from "./rules.js";
import { MEMBER_KINDS, ROLE_HELP, aboveTheScope, accessorName, describe, endsOfPoint,
         glyphSvg, linkTitleNode, memberCode, memberMarkSvg, memberParts, roleOf, scopeGate,
         seatsOfFront, signInSvg } from "./canvas.js";
import { entityFiles, isShared } from "./project.js";

// The findings of a page with no checker behind it: two empty maps, so the card never tests
// for them.
const NO_PROBLEMS = {entities: new Map(), links: new Map()};

function entityNamed(design, name) {
    return (design.entities || []).find((one) => one.name === name) || null;
}

// A sentence with its code set apart. A run between backticks is a chip: a scope written
// `<admin>` takes the scope chip below, and anything else (a name, `Session.login()`, a file)
// takes the code chip. Everything outside the backticks is plain text.
export function said(text) {
    const parts = String(text || "").split("`");
    const out = document.createDocumentFragment();
    parts.forEach((part, index) => {
        if (!part) {
            return;
        }
        if (index % 2 === 0) {
            out.append(part);
            return;
        }
        const scope = /^<([\w-]+)>$/.exec(part);
        out.append(scope ? scopeChip(scope[1], false) : codeChip(part));
    });
    return out;
}

// A name or a line of code inside a sentence, in a box of its own.
function codeChip(text) {
    const chip = document.createElement("code");
    chip.className = "tip__ident";
    chip.textContent = text;
    return chip;
}

// One typed name, as a contract declares it (`int id`), in the syntax colours the member rows
// use.
function typedChip(type, name) {
    const chip = document.createElement("code");
    chip.className = "tip__ident";
    const kind = document.createElement("span");
    kind.className = "tip__tok tip__tok--type";
    kind.textContent = type;
    const called = document.createElement("span");
    called.className = "tip__tok tip__tok--name";
    called.textContent = name;
    chip.append(kind, " ", called);
    return chip;
}

function tipRow(label, value) {
    const row = document.createElement("div");
    row.className = "tip__row";
    const name = document.createElement("span");
    name.className = "tip__label";
    name.textContent = label;
    const shown = document.createElement("span");
    shown.className = "tip__value";
    shown.append(said(value));
    row.append(name, shown);
    return row;
}

// The same row with a value built from parts, so a scope inside it can be the chip below and
// not a quoted word in the middle of a sentence.
function tipRowOf(label, ...parts) {
    const row = tipRow(label, "");
    const value = row.querySelector(".tip__value");
    value.append(...parts);
    return row;
}

// A scope, written as the `export:` block gates a member (`<admin>`), set apart as a chip.
// `raised` is a member gated above its connect point, which takes the warning colour the row
// on the canvas takes.
function scopeChip(scope, raised) {
    const chip = document.createElement("span");
    chip.className = `tip__scope${raised ? " is-scoped" : ""}`;
    chip.textContent = `<${scope}>`;
    return chip;
}

// What the `<scope>` after a row means, said once under a list that has one.
function scopeKey() {
    const note = document.createElement("p");
    note.className = "tip__help tip__help--key";
    note.append(scopeChip("scope", false),
                " after a member is the scope a caller must hold to reach it.");
    return note;
}

// A labelled rule across the card, between its parts.
function tipSection(label) {
    const row = document.createElement("div");
    row.className = "tip__section";
    row.textContent = label;
    return row;
}

// One end of a link, named and coloured as the role it plays, in the colours the drawing puts
// on the entities at the same moment.
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

// One member of a contract, painted as the canvas paints it: the mark for its kind, then the
// declaration in the file pane's syntax colours.
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

// What one member carries, in full. The canvas has room for the types only, so the names are
// here: a row's roles are what a consumer's delegate reads, and a call's parameters are what
// a caller supplies.
function partsRow(member) {
    if (member.kind === "prop") {
        return tipRowOf("holds", "One ", typedChip(member.type || "var", member.name || ""));
    }
    const typed = (member.kind === "model" ? member.roles : member.params) || [];
    const chips = typed.flatMap((part, index) => [index ? ", " : "",
                                                  typedChip(part.type || "var", part.name)]);
    if (member.kind === "model") {
        return typed.length ? tipRowOf("rows carry", ...chips)
                            : tipRow("rows carry", "No roles yet, so no part of a row crosses");
    }
    return typed.length ? tipRowOf("takes", ...chips) : tipRow("takes", "Nothing");
}

export function tipFor(design, what, {problems = NO_PROBLEMS, palette = []} = {}) {
    const box = document.createElement("div");
    // One member of one contract, asked for by pointing anywhere on its row: the mark or the
    // name and prototype beside it. The canvas writes the row short (types without names, no
    // word for the kind), and this says the rest, about the two entities at the ends of this
    // link.
    if (what.kind === "member") {
        const link = (design.links || []).find((one) => one.name === what.link);
        const member = ((link || {}).members || [])
            .find((one) => one.name === what.name);
        if (!member) {
            return null;
        }
        const meaning = MEMBER_KINDS[member.kind] || MEMBER_KINDS.prop;
        const ends = endsOfPoint(link);
        const head = document.createElement("div");
        head.className = "tip__head tip__head--link";
        head.append(memberMarkSvg(member.kind));
        // The member as its line of code, painted as the file pane and the canvas paint it.
        head.append(memberCode(member));
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = meaning.name;
        head.append(kind);
        box.append(head);
        // The two ends this member travels between, in the colours the drawing uses while it
        // is hovered.
        box.append(tipParty(design, "owner", link.owner ? [link.owner] : []));
        box.append(tipParty(design, "consumer", link.consumers || []));
        box.append(tipSection("what it carries"));
        box.append(partsRow(member));
        if (member.kind === "slot") {
            box.append(tipRow("answers", member.type
                ? `\`${member.type}\`, so the call resolves with a value`
                : "Nothing, so the call is made and not waited on"));
        }
        // What reaches this member is its own gate, or the point's where it names none. Only a
        // member gated above its point is described as held back.
        const raised = aboveTheScope(member, link);
        const gate = member.scope || link.scope;
        box.append(raised
            ? tipRowOf("scope", scopeChip(member.scope, true),
                       " on this member. Only a caller holding it reaches it.")
            : (gate
                ? tipRowOf("scope", scopeChip(gate, false),
                           " from the connect point, the same as its other members.")
                : tipRow("scope", "None. Any caller reaches it, anonymous included.")));
        box.append(tipHelp(meaning.says(ends.owner, ends.consumers)));
        if (raised) {
            const note = tipHelp("");
            note.append("Raised above ",
                        link.scope ? scopeChip(link.scope, false) : "the connect point's scope",
                        ", so only this member is held back from callers of ",
                        codeChip(link.owner), ".");
            box.append(note);
        }
        return box;
    }
    // One scope on a front's back: who answers callers holding it.
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
        box.append(tipRow("on", `\`${what.name}\`, which fronts for the entities behind it`));
        if (seat.tier) {
            box.append(tipParty(design, "owner", [seat.tier]));
            box.append(tipParty(design, "consumer", [what.name]));
        } else {
            box.append(tipRow("answered by",
                              "Nobody yet, so a caller of this scope is handed nowhere"));
        }
        box.append(tipHelp(seat.tier
            ? `A browser holding \`<${what.scope}>\` reaches \`${what.name}\` and is served `
              + `by \`${seat.tier}\`. It never learns that \`${seat.tier}\` exists: it `
              + `writes \`${accessorName(what.name)}\`, whoever is behind it.`
            : `Drag from here to the entity that serves callers holding \`<${what.scope}>\`, `
              + "or from that entity to here. Both draw the same routing."));
        return box;
    }
    // The sign-in mark, on an edge or beside a line from one to a browser: what happens,
    // where the secrets stay, what the browser holds, and who decides the scope.
    if (what.kind === "signin") {
        const edge = entityNamed(design, what.name);
        if (!edge) {
            return null;
        }
        const head = document.createElement("div");
        head.className = "tip__head tip__head--edge";
        head.append(signInSvg());
        const title = document.createElement("span");
        title.textContent = "Sign-in";
        head.append(title);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = `on ${edge.name}`;
        head.append(kind);
        box.append(head);
        box.append(tipRow("started by", "`Session.login()` in a client, which sends the "
                                        + "browser to this edge"));
        box.append(tipRow("runs", "The OAuth exchange with the provider (PKCE, a state it "
                                  + "checks, the ID token verified), here on the edge"));
        box.append(tipRow("keeps", "The client secret and the provider's tokens. Neither "
                                   + "ever reaches a browser"));
        box.append(tipRow("hands back", "A session cookie the page script cannot read, and "
                                        + "nothing else"));
        box.append(tipRow("decides the scope", "The mapping hook that `identity.mapping` "
                                               + "names in `synqt.yaml`"));
        box.append(tipHelp("Every scope above the default is reached through here. Every "
                           + "gated connect point, gated member and extra bundle depends on "
                           + "this mark. Under `synqt dev`, the development sign-in and the "
                           + "scope picker replace the provider."));
        return box;
    }
    // A box around a group of entities. Its name is on the canvas and its meaning is here.
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
    // The break on a line that reaches a front nothing routes to: what is wrong, and the
    // gesture that fixes it.
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
        box.append(tipHelp(`\`${what.consumer}\` hands its callers to the entities behind `
                           + `it, and no scope is handed to \`${link.owner}\`. Nothing travels `
                           + "down this link, because nobody is ever routed to its end."));
        box.append(tipHelp("Drag from the cross onto a scope on the front's back to say whose "
                           + "callers it serves. Press it to select the line."));
        return box;
    }
    // A row in the rail describes the entity it adds, in the same card the node on the canvas
    // opens.
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
        // A gate keeps a client's colour and takes the barrier's glyph, as its node does.
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
        // `web_edge` as the configuration writes it, read as `web edge`.
        kind.textContent = describe(entity).replace(/_/g, " ");
        head.append(kind);
        if (gate) {
            box.append(tipRow("the gate", `What \`${gate}\` serves a session that has not `
                                          + "signed in. A signed-in session is served another "
                                          + "bundle and cannot fetch a file of this one."));
        }
        box.append(tipRow("reachable from",
                          role === "client" ? "The person using it"
                          : (role === "edge" ? "The internet, and only over TLS"
                                             : "The entities on its consumer lists, and "
                                               + "nothing else")));
        // The mark the canvas draws on this node, in words. Without it nobody leaves the
        // default scope, so every member gate refuses everybody.
        if (runsSignIn(entity)) {
            box.append(tipRow("signs people in",
                              "It runs the OAuth exchange, keeps the tokens and the "
                              + "sessions, and hands the browser a cookie. `Session.login()` "
                              + "in a client reaches this."));
        }
        // How many of it there are, which decides whether a Source holds one caller's state or
        // everybody's.
        if (role !== "client") {
            box.append(tipRow("how many", isShared(entity)
                ? "One, for everybody. Every caller still arrives with a `Caller` of "
                  + "their own"
                : "One per caller, holding only what is theirs"));
        }
        // The links this entity is at either end of, in the role colours a hovered link
        // paints its ends with.
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
            box.append(tipRow("consumes",
                              uses.map((link) => `\`${link.owner}\``).join(", ")));
        }
        if (!owns.length && !uses.length) {
            box.append(tipRow("on the mesh", "Nothing reaches it and it reaches nothing"));
        }
        const front = frontsOf(design).get(entity.name);
        if (front) {
            const wired = seatsOfFront(front).filter((seat) => seat.tier);
            box.append(tipRow("hands on", wired.length
                ? wired.map((seat) => `\`<${seat.scope}>\` to \`${seat.tier}\``).join(", ")
                : "Nothing yet, so it hands nobody anywhere"));
        }
        const files = entityFiles(design, entity);
        box.append(files.length ? tipFiles(files) : tipRow("files", "None yet"));
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

    // The two ends first, in the two role colours.
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
        ? `\`${accessorName(link.owner)}\`, in every consumer's QML`
        : "Nothing yet: the owner is the name"));
    const browser = (what.consumer ? [what.consumer] : consumers).some(
        (name) => roleOf(entityNamed(design, name) || {}) === "client");
    box.append(tipRow("carried over", browser
        ? "The browser link, over TLS the edge terminates"
        : (link.transport === "local"
            ? "A local socket, so the caller is trusted by colocation"
            : "Mutual TLS, verified against the project CA")));
    box.append(link.scope
        ? tipRowOf("scope", scopeChip(link.scope, false),
                   " on the whole point. A browser below it never acquires it.")
        : tipRow("scope", "None on the point, so any session acquires it"));
    const behind = seatsOfFront(frontsOf(design).get(link.owner))
        .filter((seat) => seat.tier);
    if (behind.length) {
        box.append(tipRow("handed on to",
                          behind.map((seat) => `\`<${seat.scope}>\` to \`${seat.tier}\``)
                              .join(", ")));
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
        if (members.some((member) => scopeGate(member, link))) {
            box.append(scopeKey());
        }
    } else {
        box.append(tipHelp(link.owner
            ? `Nothing crosses it yet. Tick what \`${link.owner}\` declares onto the `
              + "contract. Nothing undeclared ever crosses, whatever anyone writes."
            : "Nothing crosses it yet. Nothing undeclared ever will."));
    }
    box.append(...tipFindings(problems.links.get(link.name) || []));
    return box;
}

// Every file an entity is made of, one per line, each a code chip.
function tipFiles(files) {
    const row = tipRow("files", "");
    const value = row.querySelector(".tip__value");
    value.classList.add("tip__files");
    for (const file of files) {
        value.append(codeChip(file.name));
    }
    return row;
}

function tipHelp(text) {
    const note = document.createElement("p");
    note.className = "tip__help";
    note.append(said(text));
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

// Where the card opens, beside the pointer and inside the window. `card` is already shown, so
// its size can be measured. An entity's card opens beside the whole node, so it does not cover
// the files the node lists. `root` is the drawing the node is found in.
export function placeTip(card, what, at, root) {
    const box = card.getBoundingClientRect();
    const node = what && what.kind === "entity" && root
        ? root.querySelector(`[data-entity="${CSS.escape(what.name)}"]`) : null;
    const around = node ? node.getBoundingClientRect()
                        : {left: at.x, right: at.x, top: at.y};
    const gap = node ? 12 : 18;
    const x = around.right + gap + box.width > window.innerWidth
        ? around.left - gap - box.width : around.right + gap;
    const y = Math.min(node ? around.top : at.y + 12, window.innerHeight - box.height - 8);
    card.style.left = `${Math.max(8, x)}px`;
    card.style.top = `${Math.max(8, y)}px`;
}

// What the pointer is on, as the card's own question.
//
// Read off the drawing, not the document: every part of it carries the name of what it is,
// so this is one `closest` per kind, smallest first. A member's row is on a line, and a line
// is in a box.
export function whatIsUnder(target) {
    if (!target || !target.closest) {
        return null;
    }
    // One member of a contract, before the link it is written beside.
    const member = target.closest("[data-member]");
    if (member) {
        const holder = member.closest("[data-link]");
        // Which line the row is written beside, as well as its point: the block is drawn
        // once per consumer, and the line under the pointer lights too.
        return {kind: "member", name: member.dataset.member,
                link: holder ? holder.dataset.link : "",
                consumer: holder ? (holder.dataset.consumer || "") : ""};
    }
    // The break on a line that reaches a front nothing routes to, before the link it sits on.
    const broke = target.closest("[data-break]");
    if (broke) {
        return {kind: "break", name: broke.dataset.break,
                consumer: broke.dataset.breakConsumer || ""};
    }
    // The sign-in mark, before the node it rides on and the line it is written beside.
    const signin = target.closest("[data-signin]");
    if (signin && signin.dataset.signin) {
        return {kind: "signin", name: signin.dataset.signin};
    }
    // A scope on a front's back, before the node it is drawn in.
    const seat = target.closest("[data-seat]");
    if (seat) {
        return {kind: "seat", name: seat.dataset.seat, scope: seat.dataset.scope};
    }
    const entity = target.closest("[data-entity]");
    if (entity) {
        return {kind: "entity", name: entity.dataset.entity};
    }
    // The contract icon before the lines it is drawn over.
    const contract = target.closest("[data-contract]");
    if (contract) {
        return {kind: "contract", name: contract.dataset.contract};
    }
    const link = target.closest("[data-link]");
    if (link) {
        // A broken line answers as its break wherever the pointer is on it, so its card never
        // describes it as working.
        if (link.dataset.broken) {
            return {kind: "break", name: link.dataset.link,
                    consumer: link.dataset.consumer || ""};
        }
        return {kind: "link", name: link.dataset.link, consumer: link.dataset.consumer || ""};
    }
    // A box's name, last, because everything inside a box answers first. The box's meaning
    // is stored on its name.
    const zone = target.closest("[data-zone-title]");
    return zone
        ? {kind: "zone", name: zone.textContent, note: zone.dataset.note || ""}
        : null;
}
