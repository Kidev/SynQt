<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The designer

A SynQt system is a handful of entities and the connect points between them. The designer
draws it live, with entities as nodes, connect points as the lines between them, and a panel
for what each one carries.

There are two ways to open it, and both are the same page.

- **In a project.** `synqt design` serves it on this machine and opens it. What you draw
  is that project. Apply writes `synqt.yaml` and the QML files a new entity or connect
  point needs.
- **[On this site](/designer/).** The same editor with nothing behind it. Draw a system,
  press Export and take it as a project, and you get a zip. Nothing is installed and nothing
  is read off your machine, because there is no machine on the other end of the page.

The second is the one to open first if you have not installed anything yet. Unzip what it
gives you over a project made with `synqt new`, or keep it as a sketch and open the real
thing later.

A link can hand you a system rather than an empty canvas: the button under
["what it looks like"](index.md) on the front page opens
[that project](/designer/#example=demo) in the designer, laid out and ready to be pulled
apart. It opens with that project's own files, the ones the page reads out underneath it,
rather than with an empty entity per node.

**Examples** in the bar is the same list without the link: every project the tutorials
build, each one opening the way this one does. Over a real project it is not offered, since
the project on disk is what that page is editing.

It opens as a starting point rather than as a fixed page. From then on it is an ordinary
project. Move things,
add things, edit the files, and what you left is what comes back the next time you follow the
same link. Clear starts over.

**Export** is how a design leaves this page, in either of the two forms it has.

- **Export as image** hands you the drawing as a PNG, the whole design at its own size with
  a margin round it, over the page colour or over nothing, which is the picture to paste
  into a document or an issue. It is the canvas as it is drawn, so it says what the project
  says.
- **Export as project** hands you the files, a zip of the `synqt.yaml` and the QML the
  design describes, ready to unzip over a project made with `synqt new`. On the copy on this
  site this is the way to keep what you drew, because nothing here is written to a disk.

## What you can draw

The rail on the left is the entity palette, and it is the list from
[entities](entities.md): a client, a web edge, the three types that come with an engine
behind them (relational, cache, document), the api and jobs types, which come with a
helper and no engine, the [monitor](monitoring.md), and a plain service you write
yourself. Each row carries the glyph the canvas draws that entity with, and hovering one
says what that kind of entity is for and when you would reach for it. The same line
appears in the panel once one is on the canvas.

Drag a row onto the canvas to put an entity where you dropped it. Dragging is the only way
one arrives, so an entity is always somewhere you chose rather than somewhere a column had
room.

Every entity arrives with its own file, before it owns or consumes anything. A client's is its
window, `client/app/Main.qml`; every other entity's is named after it, `web/edge/Edge.qml`. That
one file is the entity: what it exports and the state behind it. An entity that mints a Source
per caller and still needs something shared between them writes a `pragma Shared` file of its
own beside it, under a name it chooses.

The monitor is the row where one node is worth four things: the entity that keeps the
history, a console client, the sign-in page an anonymous visitor is handed instead of that
console, and the `monitoring.entity` line that makes every other entity report at all.
Three of those are files, and the console's is three hundred lines of QML, so drawing one
here writes all of them: the copy on this site carries the same
templates [`synqt add entity ops --type monitor`](monitoring.md) uses, and the project you
export runs without being finished by hand.

A monitor is drawn like any other entity, and the canvas leaves it
unwired on purpose: the link every service opens to it comes from the one `monitoring.entity`
line rather than from a line anybody draws. A second monitor beside it is marked, because
that line names one entity and nothing would ever report to the other.

The boxes behind the nodes are the three sides of a system, and they are drawn from what each
entity is rather than from where it sits: CLIENTS, FACES THE INTERNET, and MESH. Hovering a
box's name says what it means. Under each node is the file to open next: `client/app/Main`,
`web/edge/Edge`, `db/relational/store/Store`.

Entities settle onto a grid as you drag them, so a drawing lines up without anyone nudging
it, and a box picked up by its own background carries everything in it by the same step.

Hovering anything says the rest. An entity's card gives what it is, what can reach it, the
connect points it owns and consumes, and its files. A connect point's gives its owner, its
consumers, how it is carried, and every member that crosses it. Anything the rules have
against it is on the same card.

A connect point is drawn from the entity that owns it to the one that consumes it.
Bring the pointer near a node and its rim fills with handles. Drag any of them and drop the
line on the consumer, so a point reaching left leaves from the left. That direction is the whole meaning of the line, so it is the thing the canvas asks
you to say first, it is drawn as a filled cap on the owner and an arrowhead on the consumer,
and the owner names it. Dropping a line from `edge` onto `app` gives you the connect point
`edge` exports, carrying the `Edge` type and implemented in
`web/edge/Edge.qml`. There is nothing to name. Drawing a second line out of `edge`
adds a consumer to the one point it already exports rather than making another.

Drop the line on empty canvas instead and the palette opens there. Pick a kind and that entity
is made where you let go, consuming the point in the same gesture.

Along each line are the members that cross it, one to a row and all starting in the same
column, inside an outlined box so a row landing on a zone edge or on another line is still
readable. Each row is the member and nothing else, `int highest` or `placeBid(int): bool`,
coloured the way the same contract is coloured in the file pane below, a shade back. The
small mark at the start of the row says which of the four kinds it is. Hover anywhere on a
row, the mark or the prototype, and it says the rest, the kind, the full declaration with
the parameter names the row has no room for, what a model's rows carry, what a call answers
with, and which callers reach it. A member held above the point's own scope says so on the
row, in the notation the `export:` block gates it in, `erase(int) <admin>`, after the
declaration and in the warning colour, so which scope is a thing to read rather than a
thing to go and ask for.

Hovering anything on the canvas lights it, in a colour of its own rather than the one selection
uses, so moving the pointer across a busy drawing never costs sight of what you are working on.
A line lights with the contract it carries and, where it lands on a front, the scope it answers
for, so it shows what crosses and who serves it, without tracing either by eye.

Where two lines run in opposite directions between the same pair of entities, they bow
apart into separate curves so each keeps its own members and its own click.

A web edge that hands its callers on is a [front](programming-model.md), and the switch for
it is on the edge's own panel, with the rest of what an edge does. A front is drawn as a
wedge rather than a disc. Everyone arrives at the nose facing the browser, and along its
back is a seat per scope, each named inside the shape. Drag between a seat and an entity, or
between the name of one and an entity, either way round, to say
which entity serves that scope's callers. The seat lights up as the line comes over it, and
the connect point the front needs to reach that entity is drawn at the same time. Let a line
go anywhere else on the wedge and it asks which scope you meant. Dragging a seat onto empty
canvas takes that scope off, and so does deleting the link it was wired along, because the
routing and the link are one declaration, so they go together.

Throwing the switch stops the edge answering its own connect point, so every link already
running into it now carries nobody until a scope names the entity at the other end. Those
links stay on the canvas and are drawn as what they have become, severed three quarters of
the way along, under a red cross and the word broken. Pointing anywhere along such a line
says the same thing the cross does, and the dash that runs along a hovered link stops at the
break, because past it nothing travels. The cross is the fix as well as the diagnosis. Drag
from it onto a scope on the front's back and that scope is served by the entity the line came
from. The line you pull leaves the owner's own connect point, because what the gesture does
is wire this link the way it would have been wired in the first place. Pressing the cross
instead of dragging it selects the line, the same as pressing the line itself.

Selecting a node or a line opens the panel on the right, which is where the rest lives, an
entity's provider, a connect point's consumer list, and what crosses it. The consumer list
is the authorization. An entity that is not on it is refused the replica.
[Security](security.md) is where that is spelled out.

Both side panels fold. The chevron at the top of each one, level with the line that says what
the panel is, puts it away. The panel goes, the drawing takes the width, and all that is left
of it is a tab against the edge of the window. Press the tab and the panel comes back. On a
window too narrow for three columns they start folded and open over the canvas, so a phone
shows the design rather than two columns of furniture with a sliver between them.

Each control in the panel says what it is behind the `?` beside it, and each section says
what the group of them is for behind the `?` on its heading. The marks appear when the pointer
is on that section and the words when the pointer is on the mark, so what is on screen is the
settings rather than a paragraph before each of them, and the answer you get is the answer
about the setting you were pointing at.

Every member is written the way the file that declares it writes it, in the same colours the
pane below uses, `prop bool loaded` or `slot bool placeBid(int amount)`. A connect point is
named by its two ends with an arrow between them, `edge -> app`, on its own panel, on the
button that opens it, and on the card that hovering it opens.

Under **Wired to**, an entity's panel names who is at the other end of every line it is on,
the entities that consume the connect point it owns, and the owner of every point it consumes.
Each of those names is a button that selects that entity, so following a line is a click
rather than a hunt across the canvas.

Clicking a point's only line opens the point, because with one consumer the line and the
point are the same thing. Where a point has several, clicking one opens that consumer, and
the panel names the point it belongs to with a button that goes there.

An entity's panel is where its members are declared, listed as the lines they are,
`property int highBid` or `function placeBid(amount: var)`. Clicking one opens the controls
over that line, and every part of it that comes out of a fixed list is chosen from that
list, the kind, the type, a parameter's type, a model's roles. Only names are typed. What
you declare there is written into the entity's own file, which is the same thing as typing
the line into the file below. Editing it rewrites that line and leaves the body of a
function alone. A model is declared there too, with the other three, and it is the one
kind written onto the point rather than into the file, because QML has no declaration form
for one.

What an entity is the panel states and does not offer. A database is a database because
that is the row it was dragged from, and everything drawn against it since means what it
means because of that. Turning one into a client in a drop-down would keep the name, the
place and the connect points while changing the thing underneath them. Delete it and drag
the one you wanted.

Right-clicking a node or a line opens the same things over it, edit, rename, delete.
Double-clicking a node renames it and <kbd>Delete</kbd> removes what is selected. Renaming an
entity carries the new name into every connect point that referred to the old one, including
the one it exports, and deleting one takes that point with it.

The two arrows at the head of the buttons on the right of the bar go back over what you have
done and forward again, and so do <kbd>Ctrl</kbd>+<kbd>Z</kbd> and
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd>.
Everything an edit changes is in the design, so a step back is the whole design as it was.
Undoing a delete brings the entity back with the lines that went with it. A drag is one step
however many times the pointer moved, and typing into a file is one step for that file rather
than one per letter. With the caret in the file pane those keys are the pane's own undo, over
the text.

## The scopes a project declares

With nothing picked, the panel is the project's scope vocabulary, one row per scope, typed
in place to rename it, the two arrows to move it, and an adder under the list. It lives
there because a scope belongs to no entity and no link, and every gate in every other panel
chooses from this list.

The order matters twice. It is the authority ranking under `scopes.hierarchical`, so a
scope satisfies every scope below it. And it is what the mapping hook's generated
`Scope` enum counts from, so moving a row renumbers the vocabulary and every hook is
regenerated against the new numbers (see
[the identity mapping hook](authentication.md#the-identity-mapping-hook)). The first scope is
what a caller with no session holds.

Renaming a scope renames it here and nowhere else. Whatever was gated on the old name keeps
naming it, the gate on a connect point, the gate on one member of its contract, which entity
a front hands each scope to, which bundle an edge serves each scope, and the member the
mapping hook answers with. Each of those is then a scope the project does not declare, so
[the change sheet](#nothing-is-written-until-you-have-read-it) names every one of them
and Apply stays refused until you say what they hold now.

Working out that a row was typed over, rather than one scope removed and another added,
means guessing it from the list before and the list after, and a guess that lands wrong edits
a gate, or somebody's mapping hook, on a coincidence. A refusal that names the exact gate is
something to act on. A file quietly rewritten is not. A scope that is still
named somewhere cannot be removed either, and there the remover says where it is still named
before you press it.

## The same project as text

The pane under the canvas is the project this drawing is, open from the start, `synqt.yaml`,
which carries what crosses every connect point, the QML of every entity under its own
directory, and a `schema.sql` beside every relational entity. It is rebuilt from the drawing on every edit, so it can never be showing an older
design than the canvas above it. The chevron on its bar collapses it to a strip along the
bottom, carrying the word Files and the control that brings it back, and the whole strip
opens it again.

It is a real editor, [CodeMirror](https://codemirror.net/), so it has the things a pane you
type code into is expected to have, numbered lines, a matched brace, a visible selection, an
undo of its own per file. The colours are the editor's own reader, the same one that paints
the members written along a link on the canvas, so QML, `synqt.yaml`, a contract's `export:`
block inside it, and a `schema.sql` are each read as what they are.

Selecting an entity or a connect point on the canvas opens its file, and opening a file selects
the entity it belongs to, so the two views are never on different subjects.

Every file here is one you type into, and what you type is the design. A property
declared in an entity's QML is one that entity declares. An entity written into `synqt.yaml`
is one on the canvas. The project opens read-only, and **Edit files**, at the end of the bar
over the tree, opens all of it for typing until you press it again. Following a declaration
from one entity into another is three files in a minute, and a lock picked again on each of
them would be three interruptions in the middle of one thought. There is no save. What you type is
in the design as you type it, and the design still reaches the project only through the change
set you review and apply.

Declare a property, a signal or a function in an entity's file and the entity declares it,
which is exactly what adding it in the panel does:

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

Declaring is not exporting. A contract is the list of what an owner has agreed to say to
somebody else, so what crosses a connect point is what is ticked on it, and a fresh point
carries nothing. Tick the new member on the point and it crosses.

Reach for something another entity owns, and the connect point that would have to carry it
is drawn for you, with the entity that owns it, you on its consumer list, and the member you
reached for, because code that is already written is somebody having said so:

```qml
// in client/app/Main.qml
property int score: Server.score
```

draws the connect point the web edge owns, with the client on its consumer list and
`prop var score` crossing it. This
is [`synqt infer`](#reading-the-contracts-back) as you type, and it works on the copy on this
site too, where there is no CLI behind the page at all. A member nothing gave a type to comes
back `var` for you to name.

For the same reason, drawing a link to an entity whose code already calls into the owner
starts the contract with exactly what that code calls, and nothing else.

Reading is additive. A declaration corrects a member that crosses. A member with no
declaration is left alone, because half-typed text is not an instruction to delete a
contract. Removing a member is the panel's `x`. A name is typed one letter at a time, and
the editor follows it. Renaming a member in the file renames it on every contract carrying
it rather than leaving one member per letter behind.

A size (`string[120]`) is not part of a declaration and cannot be, because QML has no type with a
limit in it, and `property string[120] message` is a syntax error rather than a property with
a limit. It is part of the contract, so it is set on the connect point, beside the scope, and
it is what the owner-side boundary refuses anything longer than.

Putting the caret on a line points the canvas at what that line is about, so a file you are
reading and the drawing stay on the same subject.

## The rules are live

The findings under the palette are a subset of `synqt check`, run in the page on every edit,
and the canvas paints them. A connect point the deployment would refuse goes red while you
are still drawing it rather than in a build four steps later. A client consuming a point
that is not owned by a web edge, a point owned by an entity that is not there, an owner
listed as its own consumer, two entities with one name, a link put on a local socket, and an
edge switched to hand its callers on with nothing behind it are all in that subset.

Each one is marked where it is, and not only in the list. The entity or the connect point it is
about carries a small mark, and hovering that mark says what the finding is and what to do
about it. Clicking the line in the list picks the same thing out on the canvas.

The page's copy of the rules is never a second opinion. Every rule it paints is checked
against the command line's verdict for the same topology, case by case, by the test suite.
Where the two could ever disagree, the one that decides is the server's. Apply runs the
real `synqt check`, and a design it refuses cannot be applied.

## Nothing is written until you have read it

Drawing writes nothing. When the design says what you mean, press Review. The editor asks
what applying it would do and shows the whole change set as a diff, file by file. Apply then
names that change set by its digest, and the server refuses anything else. If you edit after
reviewing, the plan is void and Review comes back.

This is why a project that does not pass `synqt check` still opens. A broken topology is
what you came to fix, so opening is not gated. The verdict arrives with the project,
painted on the canvas, and Apply is what refuses it.

## Reading the contracts back

"Infer from the sources" is [`synqt infer`](build-system-and-cli.md#the-synqt-command-line-tool)
on the canvas. It reads the project's own QML, both ends of every link, and fills each
connect point with the members the code already uses, the props the owner's Source assigns,
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

The copy on this site has none of that to do. It talks to no server, so it holds nothing.
Close the tab and the drawing is gone. Export it as a project first. The browser asks on
the way out, once, whichever way you leave. The mark in the corner is the way back to the
rest of the site.

See [build system and CLI](build-system-and-cli.md#the-synqt-command-line-tool) for the
command, [project layout and config](project-layout-and-config.md) for what the file it
writes means, and [getting started](getting-started.md) for the shortest path from an empty
directory to something running. The [developer guide](development.md#adding-a-rule-to-the-designer)
covers the editor from the other side, including what moves when a rule is added to it.
