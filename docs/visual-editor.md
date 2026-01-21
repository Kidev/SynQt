<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The designer

A SynQt system is a handful of entities and the connect points between them. The designer
draws it live: entities as nodes, connect points as the lines between them, and a panel
for what each one carries.

There are two ways to open it, and both are the same page.

- **In a project.** `synqt design` serves it on this machine and opens it. What you draw
  is that project: Apply writes `synqt.yaml` and the QML files a new entity or connect
  point needs.
- **[On this site](/designer/).** The same editor with nothing behind it. Draw a system,
  press Download, and you get the project as a zip. Nothing is installed and nothing is
  read off your machine, because there is no machine on the other end of the page.

The second is the one to open first if you have not installed anything yet. Unzip what it
gives you over a project made with `synqt new`, or keep it as a sketch and open the real
thing later.

A link can hand you a system rather than an empty canvas: the button under
["what it looks like"](index.md) on the front page opens
[that project](/designer/#example=feed) in the editor, laid out and ready to be pulled
apart.

## What you can draw

The rail on the left is the entity palette, and it is the list from
[entities](entities.md): a client, a web edge, the four types that come with an engine
behind them (relational, cache, document, api), the jobs type, and a plain service
you write yourself. Each row carries the glyph the canvas draws that entity with, and hovering
one says what that kind of entity is for and when you would reach for it; the same line
appears in the panel once one is on the canvas.

Drag a row onto the canvas to put an entity where you dropped it. Dragging is the only way
one arrives, so an entity is always somewhere you chose rather than somewhere a column had
room.

Every entity arrives with its own file, before it owns or consumes anything. A client's is its
window, `client/app/Main.qml`; every other entity's is named after it, `web/edge/Edge.qml`. That
one file is the entity: what it exports and the state behind it. An entity that mints a Source
per caller and still needs something shared between them writes a `pragma Singleton` of its own
beside it, under a name it chooses.

The boxes behind the nodes are the three sides of a system, and they are drawn from what each
entity is rather than from where it sits: CLIENTS, FACES THE INTERNET, and MESH. Hovering a
box's name says what it means. Under each node is the file to open next: `client/app/Main`,
`web/edge/Edge`, `db/relational/store/Store`.

Entities settle onto a grid as you drag them, so a drawing lines up without anyone nudging
it, and a box picked up by its own background carries everything in it by the same step.

Hovering anything says the rest. An entity's card gives what it is, what can reach it, the
connect points it owns and consumes, and its files; a connect point's gives its owner, its
consumers, how it is carried, and every member that crosses it. Anything the rules have
against it is on the same card.

A connect point is drawn from the entity that **owns** it to the one that **consumes** it.
Every node has a handle on each of its four sides; drag any of them and drop the line on the
consumer. That direction is the whole meaning of the line, so it is the thing the canvas asks
you to say first, it is drawn as a filled cap on the owner and an arrowhead on the consumer,
and the owner names it: dropping a line from `edge` onto `app` gives you the connect point
`edge` exports, carrying the `Edge` type and implemented in
`web/edge/Edge.qml`. There is nothing to name. Drawing a second line out of `edge`
adds a consumer to the one point it already exports rather than making another.

Drop the line on empty canvas instead and the palette opens there: pick a kind and that entity
is made where you let go, consuming the point in the same gesture.

Along each line are the members that cross it, written the way the contract writes them:
`prop int highest`, `slot placeBid(int): bool`. That is the whole of what a line says, and it
is what a reader following one came for. A member held above the point's own scope carries a
mark; hovering it says which scope.

Where two lines run in opposite directions between the same pair of entities, they bow
apart into separate curves so each keeps its own members and its own click.

Dropping a line on a [front](programming-model.md) asks which scope's callers the entity
serves, because a front's seats are a few pixels apart and choosing one by aim is not
something a hand can do. Dragging out of a seat has never had that problem and is unchanged.

Selecting a node or a line opens the panel on the right, which is where the rest lives: an
entity's provider, a connect point's consumer list, and what crosses it. The consumer list
is the authorization: an entity that is not on it is refused the replica.
[Security](security.md) is where that is spelled out.

Clicking a point's only line opens the point, because with one consumer the line and the
point are the same thing. Where a point has several, clicking one opens that consumer, and
the panel names the point it belongs to with a button that goes there.

An entity's panel is where its members are declared, and every part of one that comes out of
a fixed list is chosen from that list: the kind, the type, a parameter's type, a model's
roles. Only names are typed. What you declare there is written into the entity's own file,
which is the same thing as typing the line into the file below; editing it rewrites that
line and leaves the body of a function alone. A model is declared there too, with the other
three, and it is the one kind written onto the point rather than into the file: QML has no
declaration form for one.

What an entity **is** the panel states and does not offer. A database is a database because
that is the row it was dragged from, and everything drawn against it since means what it
means because of that; turning one into a client in a drop-down would keep the name, the
place and the connect points while changing the thing underneath them. Delete it and drag
the one you wanted.

Right-clicking a node or a line opens the same things over it: edit, rename, delete.
Double-clicking a node renames it and <kbd>Delete</kbd> removes what is selected. Renaming an
entity carries the new name into every connect point that referred to the old one, including
the one it exports, and deleting one takes that point with it.

## The same project as text

The pane under the canvas is the project this drawing is, open from the start: `synqt.yaml`,
which carries what crosses every connect point, and the QML of every entity under its own
directory. It is rebuilt from the drawing on every edit, so it can never be showing an older
design than the canvas above it. **Hide** collapses it to the strip along the bottom, which is
also what opens it again.

Selecting an entity or a connect point on the canvas opens its file, and opening a file selects
what it is on the canvas, so the two views are never on different subjects.

The configuration and the contracts are written from the drawing, so they are read here and
edited on the canvas. **The QML is the other way round: you type into it, and what you type
is the design.** Every file opens read-only; the button beside its name unlocks the one you
want to edit. There is no save: what you type is in the design as you type it, and the design
still reaches the project only through the change set you review and apply.

Declare a property, a signal or a function in a connect point's Source and it becomes a
member of that contract, exactly as if you had added it in the panel:

```qml
Edge {
    id: root

    property bool loaded
    signal denied(reason: string)
    function load(id: int): bool {
        return;
    }
}
```

Reach for something another entity owns, and the connect point that would have to carry it
is drawn for you, with the entity that owns it, you on its consumer list, and the member you
reached for:

```qml
// in client/app/Main.qml
property int score: Server.score
```

draws `game`, owned by the web edge, consumed by the client, carrying `prop var score`. This
is [`synqt infer`](#reading-the-contracts-back) as you type, and it works on the copy on this
site too, where there is no CLI behind the page at all. A member nothing gave a type to comes
back `var` for you to name.

Reading is additive. A declaration adds or corrects a member; a member with no declaration is
left alone, because half-typed text is not an instruction to delete a contract. Removing a
member is the panel's `x`.

A size (`string[120]`) is not part of a declaration and cannot be: QML has no type with a
limit in it, and `property string[120] message` is a syntax error rather than a property with
a limit. It is part of the contract, so it is set on the connect point, beside the scope, and
it is what the owner-side boundary refuses anything longer than.

Putting the caret on a line points the canvas at what that line is about, so a file you are
reading and the drawing stay on the same subject.

## The rules are live

The findings under the palette are a subset of `synqt check`, run in the page on every edit,
and the canvas paints them: a connect point the deployment would refuse goes red while you
are still drawing it rather than in a build four steps later. A client consuming a point
that is not owned by a web edge, a point owned by an entity that is not there, an owner
listed as its own consumer, two entities with one name, and a link put on a local socket
are all in that subset.

The page's copy of the rules is never a second opinion. Every rule it paints is checked
against the command line's verdict for the same topology, case by case, by the test suite.
Where the two could ever disagree, the one that decides is the server's: Apply runs the
real `synqt check`, and a design it refuses cannot be applied.

## Nothing is written until you have read it

Drawing writes nothing. When the design says what you mean, press Review: the editor asks
what applying it would do and shows the whole change set as a diff, file by file. Apply then
names that change set by its digest, and the server refuses anything else. If you edit after
reviewing, the plan is void and Review comes back.

This is why a project that does not pass `synqt check` still opens. A broken topology is
what you came to fix, so opening is not gated; the verdict arrives with the project,
painted on the canvas, and Apply is what refuses it.

## Reading the contracts back

"Infer from the sources" is [`synqt infer`](build-system-and-cli.md#the-synqt-command-line-tool)
on the canvas. It reads the project's own QML, both ends of every link, and fills each
connect point with the members the code already uses: the props the owner's Source assigns,
the models it pushes, the signals it emits, and the slots the consumers call. A contract
you have never written arrives drawn instead of typed out.

The result is evidence rather than proof, and the page says so. A member nothing in the
QML gave a type to comes back `var`, the hint counts them, and each one is yours to open
and name. Where a
box sits on the canvas is this page's drawing rather than the project's, so inferring does
not rearrange what you have laid out.

Like everything else here, the result is a document until you review and apply it.

## Where it is served, and to whom

`synqt design` binds the loopback address only, on port 8181 (`--port` moves it), and every
request carries a token minted for that run. The token is in the fragment of the URL the
command prints, which a browser never sends to a server, so it stays out of every log and is
worth nothing once you press Ctrl-C. A page from anywhere else is refused, by name and by
origin, and the editor answers no request that arrives without the token.

The copy on this site has none of that to do. It talks to no server, so it holds nothing:
close the tab and the drawing is gone. Download it first. Leaving asks before it goes, and
the mark in the corner is the way back to the rest of the site.

See [build system and CLI](build-system-and-cli.md#the-synqt-command-line-tool) for the
command, [project layout and config](project-layout-and-config.md) for what the file it
writes means, and [getting started](getting-started.md) for the shortest path from an empty
directory to something running. The [developer guide](development.md#adding-a-rule-to-the-designer)
covers the editor from the other side, including what moves when a rule is added to it.
