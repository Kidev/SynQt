<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The designer

A SynQt system is a few entities and the connect points between them. The designer draws
it live: entities are nodes, connect points are the lines between them, and a panel shows
what each one carries.

You can open it two ways, and both show the same page:

- **In a project.** `synqt design` serves it on your machine and opens it. What you draw is
  that project, and Apply writes `synqt.yaml` and the QML files a new entity or connect
  point needs.
- **[On this site](/designer/).** The same editor with nothing behind it. Draw a system,
  press Export and take it as a project to get a zip. Nothing is installed, and nothing is
  read from your machine.

Open the site copy first if you have not installed anything yet. Unzip its export over a
project made with `synqt new`, or keep it as a sketch.

A link can open a system instead of an empty canvas: the button under
["what it looks like"](index.md) on the front page opens
[that project](/designer/#example=demo), laid out and ready to change, with the project's
own files, the ones the front page shows.

**Examples** in the toolbar lists every project the tutorials build, each opening the same
way. It is hidden over a real project, since that page edits the project on disk.

An example is a starting point. Once open, it is an ordinary project: move things, add
things, edit the files, and your changes come back the next time you follow the same link.
Clear starts over.

**Export** takes a design off the page in two forms:

- **Export as image** gives you a PNG of the whole design at its own size, with a margin,
  over the page color or a transparent background, ready for a document or an issue. It
  is the canvas as drawn.
- **Export as project** gives you a zip of the `synqt.yaml` and the QML the design
  describes, ready to unzip over a project made with `synqt new`. On the site copy, this is
  how you keep your work, because nothing is written to a disk.

## What you can draw

The rail on the left is the entity palette, matching the list in [entities](entities.md):
a client, a web edge, the three types with an engine (relational, cache, document), the api
and jobs types (a helper and no engine), the [monitor](monitoring.md), and a plain service
you write yourself. Each row shows the entity's glyph, and hovering a row says what that
kind of entity is for and when to use it. The panel shows the same line once the entity is
on the canvas.

Drag a row onto the canvas to place an entity where you drop it. Dragging is the only way
to add one, so every entity sits where you chose.

Every entity arrives with its own file, before it owns or consumes anything. A client's is
its window, `client/app/Main.qml`. Every other entity's is named after it,
`web/edge/Edge.qml`. That file is the entity: what it exports and the state behind it. An
entity that creates a Source per caller and needs shared state writes a `pragma Shared`
file beside it, under a name it chooses.

Dropping a monitor creates four things: the entity that keeps the history, a console
client, the sign-in page an anonymous visitor gets instead of the console, and the
`monitoring.entity` line that makes every other entity report. Three are files, and the
console is three hundred lines of QML, so the designer writes all of them. The site copy
uses the same templates as [`synqt add entity ops --type monitor`](monitoring.md), so the
project you export runs without manual steps.

A monitor is drawn as a square with an eye, and the canvas leaves it unwired on purpose:
the link every service opens to it comes from the `monitoring.entity` line, not from a
drawn line. Selecting it opens its console, `client/ops-console/Main.qml`. A second monitor
is marked, because that line names one entity, and nothing would ever report to the other.

For the same reason, you cannot draw a line to or from a monitor. It has no handles on its
rim, a line dropped on it is refused on the spot, and the editor leaves it out of the
consumer menu and the panel's lists. A project that arrives with such a line is not
silently changed: a point naming a monitor as a consumer is marked, and `synqt check`
refuses it, because entities report to a monitor and it reaches none of them.

The boxes behind the nodes show the three sides of a system, derived from what each entity
is, not where it sits: CLIENTS, FACES THE INTERNET and MESH. A monitor adds a fourth box,
WATCHES EVERY ENTITY, drawn around the other three, because every entity reports to it,
the edge included. That box has no fill, so the boxes inside keep their colors. Hovering a
box's name says what the box means.

Entities snap to a grid as you drag them, so a drawing lines up by itself. Drag a box by
its name to move everything in it; the two small boxes (clients, and the entity facing the
internet) can also be dragged by their background. Every box's name is a handle, so all
names behave alike.

Hovering an entity lists all its files under its name, and nothing is written there at
rest, so the canvas stays readable: `client/app/` then `Main.qml`, `User.qml` and the
other files of a client, or `db/relational/store/` then `Store.qml` and `schema.sql`.

Hovering anything opens a card with the rest. An entity's card gives what it is, what can
reach it, the connect points it owns and consumes, and every one of its files. A connect
point's card gives its owner, its consumers, how it is carried, its scope, and every member
that crosses it. Any rule it breaks appears on the same card. Code in a card (a member, a
name, a call such as `Session.login()`) is set apart in a box, and a scope is written
`<admin>`, as the contract writes it.

A web edge that runs sign-in (`identity: true`) has a small door mark on its rim. Hovering
the mark opens its own card: what starts a sign-in, what runs on the edge, what the browser
gets back, and what decides the scope. Hovering the edge, or a line from it to a client,
writes the same mark beside that line with `Session.login() signs in here`, linking the
call in the client to the entity it reaches.

A connect point is drawn from the entity that owns it to the one that consumes it. Move
the pointer near a node and its rim shows handles. Drag one and drop the line on the
consumer; a line leaving to the left starts from the left handle. The direction is the
line's whole meaning, so the canvas asks for it first, and draws it as a filled cap on the
owner and an arrowhead on the consumer. The owner names the point: a line from `edge` to
`app` is the connect point `edge` exports, with the `Edge` type, implemented in
`web/edge/Edge.qml`, so there is nothing to name. A second line out of `edge` adds a
consumer to its one point instead of creating another.

Drop the line on empty canvas instead, and the palette opens there. Pick a kind, and the
new entity appears where you let go, already consuming the point.

Each line lists the members that cross it, one per row, aligned, inside an outlined box so
a row over a zone edge or another line stays readable. A row shows only the member,
`int highest` or `placeBid(int): bool`, in the file pane's colors, slightly muted. The mark
at the start of the row shows which of the four kinds it is. Hover anywhere on a row for
the rest: the kind, the full declaration with parameter names, what a model's rows carry,
what a call returns, and its scope, set apart as `<admin>`. A member gated above the
point's own scope says so on its row, in the `export:` notation (`erase(int) <admin>`),
after the declaration and in the warning color, so you can read the scope at a glance.

Hovering anything on the canvas highlights it, in a color distinct from the selection's, so
moving the pointer over a busy drawing never hides what you are working on. A hovered line
highlights with its contract and, where it lands on a front, the scope it serves, so you
see what crosses and who serves it without tracing it by eye.

When two lines run in opposite directions between the same two entities, they bow apart
into separate curves, so each keeps its own members and its own click target.

A web edge that hands its callers on is a [front](programming-model.md); the switch is on
the edge's panel. A front is drawn as a wedge instead of a disc. Callers arrive at the nose,
facing the browser, and its back has one seat per scope, each named inside the shape. Drag
between a seat (or its name) and an entity, in either direction, to say which entity
serves that scope's callers. The seat lights up as the line passes over it, and the
connect point the front needs to reach that entity is drawn at the same time. Drop a line
anywhere else on the wedge, and it asks which scope you meant. Dragging a seat onto empty
canvas removes that scope's routing, and so does deleting its link, because the routing and
the link are one declaration.

Turning a front on stops the edge answering its own connect point, so every link already
running into it carries nobody until a scope names the entity at its other end. Those
links stay on the canvas, drawn cut three quarters of the way along, under a red cross and
the word broken. Hovering such a line says the same as the cross, and the animated dash on
a hovered link stops at the break, because nothing travels past it. The cross is also the
fix: drag from it onto a scope on the front's back, and that scope is served by the entity
the line came from. The new line leaves the owner's own connect point, wiring the link as
it would have been wired from the start. Pressing the cross without dragging selects the
line, like pressing the line itself.

Selecting a node or a line opens the panel on the right, which holds the rest: an entity's
provider, a connect point's consumer list, and what crosses it. The consumer list is the
authorization: an entity not on it is refused the replica. [Security](security.md) explains
this in full.

Both side panels fold. The chevron at the top of each, beside the panel's title, folds it
away: the drawing takes the width, and a tab stays at the window edge. Press the tab to
bring the panel back. In a window too narrow for three columns, the panels start folded and
open over the canvas, so a phone shows the design, not two panels with a sliver between
them.

Each control has a `?` that explains it, and each section heading has a `?` that explains
the group. The marks appear when the pointer is on the section, and the text when it is on
the mark, so the screen shows the settings, not a paragraph before each one, and the
explanation matches the setting you point at.

Every member is written as the file that declares it writes it, in the file pane's colors:
`prop bool loaded` or `slot bool placeBid(int amount)`. A connect point is named by its two
ends with an arrow, `edge -> app`, on its panel, on the button that opens it, and on its
hover card.

Under **Wired to**, an entity's panel names who is at the other end of each of its lines:
the entities that consume its connect point, and the owner of each point it consumes. Each
name is a button that selects that entity, so you follow a line with a click.

Clicking a point's only line opens the point, since with one consumer the line and the
point are the same. When a point has several consumers, clicking a line opens that
consumer, and the panel links to the point it belongs to.

An entity's panel lists its declared members as the lines they are:
`property int highBid` or `function placeBid(amount: var)`. Clicking one opens controls for
that line, where every part that comes from a fixed list (the kind, the type, a parameter's
type, a model's roles) is chosen from that list; only names are typed. A declaration there
is written into the entity's own file, exactly as if you typed the line in the file below.
Editing it rewrites that line and leaves a function's body alone. Models are declared there
too, but written onto the point instead of into the file, because QML has no declaration
form for a model.

The panel shows what an entity is, but does not let you change it. A database is a database
because you dragged it from that row, and everything drawn against it depends on that.
Turning it into a client from a drop-down would keep the name, the position and the connect
points while changing what they mean. Delete it and drag the one you want.

Right-clicking a node or a line offers the same actions: edit, rename, delete.
Double-clicking a node renames it, and <kbd>Delete</kbd> removes the selection. Renaming an
entity updates every connect point that referred to the old name, including its own, and
deleting an entity deletes its point too.

The two arrows at the start of the toolbar's right side undo and redo, as do
<kbd>Ctrl</kbd>+<kbd>Z</kbd> and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd>. Every edit
lives in the design, so undo restores the whole design as it was. Undoing a delete brings
the entity back with its lines. A drag is one step however far the pointer moved, and typing
into a file is one step for that file, not one per letter. With the caret in the file pane,
those keys undo the pane's text.

## The scopes a project declares

With nothing selected, the panel shows the project's scopes: one row per scope, which you
rename by typing, reorder with the two arrows, and add to below the list. It lives there
because a scope belongs to no entity or link, and every gate in every other panel chooses
from this list.

The order matters for two reasons. It is the rank under `scopes.hierarchical`, so a scope
satisfies every scope below it. And the mapping hook's generated `Scope` enum counts from
it, so moving a row renumbers the scopes and every hook is regenerated with the new numbers
(see [the identity mapping hook](authentication.md#the-identity-mapping-hook)). A caller
with no session holds the first scope.

Renaming a scope renames it here only. Everything gated on the old name keeps that name: a
connect point's gate, a member's gate, which entity a front hands each scope to, which
bundle an edge serves each scope, and the member the mapping hook returns. Each of those
now names an undeclared scope, so
[the change sheet](#nothing-is-written-until-you-have-read-it) lists them all, and Apply
stays refused until you fix each one.

The editor does not guess that a typed-over row is a rename rather than one scope removed
and another added: a wrong guess would change a gate, or someone's mapping hook, by
coincidence. A refusal that names the exact gate is actionable; a silently rewritten file
is not. You also cannot remove a scope that is still named somewhere; the remove button
says where before you press it.

## The same project as text

The pane under the canvas shows the project as files, from the start: `synqt.yaml`, which
holds what crosses every connect point, and every file of every entity in its own
directory, including the QML files beside a client's `Main.qml` and a `schema.sql` beside
every relational entity. It is rebuilt from the drawing on every edit, so it always matches
the canvas. The chevron on its bar collapses it to a strip at the bottom, labeled Files;
click the strip to open it again.

It is a real editor, [CodeMirror](https://codemirror.net/), with numbered lines, brace
matching, a visible selection and a separate undo history per file. The same reader that
colors the members along each canvas line colors the pane, so QML, `synqt.yaml`, a
contract's `export:` block inside it, and `schema.sql` are each highlighted correctly.

Selecting an entity or connect point on the canvas opens its file, and opening a file
selects its entity, so both views always show the same subject.

You can type into every file, and what you type is the design. A property declared in an
entity's QML is declared by that entity; an entity written into `synqt.yaml` appears on the
canvas. The project opens read-only. **Edit files**, at the end of the bar above the tree,
unlocks everything for typing until you press it again, since following a declaration
across entities touches several files in a minute. There is no save: what you type enters
the design at once, and still reaches the project only through the change set you review
and apply.

Declaring a property, a signal or a function in an entity's file does the same as adding it
in the panel:

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

Declaring a member does not export it. A contract lists what an owner has agreed to share,
so only the members ticked on a connect point cross it, and a new point carries nothing.
Tick the new member on the point, and it crosses.

When your code uses something another entity owns, the designer draws the connect point
needed to carry it: owned by that entity, with you as a consumer, carrying the member you
used, because written code states an intent:

```qml
// in client/app/Main.qml
property int score: Server.score
```

This draws the connect point the web edge owns, with the client as a consumer and
`prop var score` crossing it. It is [`synqt infer`](#reading-the-contracts-back) running as
you type, and it works in the site copy too, with no CLI behind it. A member without a type
comes back as `var` for you to name.

Likewise, drawing a link to an entity whose code already calls into the owner starts the
contract with exactly those calls.

Reading only adds. A declaration corrects a member that crosses; a member with no
declaration is left alone, because half-typed text is not a request to delete part of a
contract. Remove a member with the panel's `x`. Names are typed one letter at a time, and
the editor follows: renaming a member in the file renames it on every contract that
carries it, instead of leaving one member per keystroke.

A size (`string[120]`) cannot be part of a QML declaration: QML has no bounded types, and
`property string[120] message` is a syntax error. The size belongs to the contract, so you
set it on the connect point, beside the scope, and the owner's boundary refuses anything
longer.

Placing the caret on a line highlights what the line is about on the canvas, so the file
and the drawing stay on the same subject.

## The rules are live

The findings under the palette are a subset of `synqt check`, run in the page on every
edit and painted on the canvas. A connect point the deployment would refuse turns red while
you draw it, not in a build later. The subset covers: a client consuming a point no web
edge owns, a point owned by an entity that does not exist, an owner listed as its own
consumer, two entities with the same name, a link on a local socket, and a front with
nothing behind it.

Each finding is marked where it applies, not only in the list. The entity or connect point
carries a small mark; hovering it says what is wrong and how to fix it. Clicking the
finding in the list selects it on the canvas.

The page's rules never contradict the command line: the test suite checks every rule it
paints against the CLI's verdict for the same topology, case by case. If they ever
disagreed, the server would decide: Apply runs the real `synqt check`, and a design it
refuses cannot be applied.

## Nothing is written until you have read it

Drawing writes nothing. When the design says what you mean, press Review: the editor
shows the whole change set as a diff, file by file. Apply then sends that change set's
digest, and the server refuses anything else. If you edit after reviewing, the plan is void
and Review returns.

So a project that fails `synqt check` still opens: you may have opened it to fix exactly
that. The verdict arrives with the project, painted on the canvas, and Apply is what
refuses it.

## Reading the contracts back

"Infer from the sources" is
[`synqt infer`](build-system-and-cli.md#the-synqt-command-line-tool) on the canvas. It reads
the project's QML at both ends of every link and fills each connect point with the members
the code already uses: the props the owner's Source assigns, the models it pushes, the
signals it emits, and the slots the consumers call. A contract you never wrote appears
drawn.

The result is a best guess, and the page says so. A member the QML gives no type comes back
as `var`; the hint counts them, and you open and name each one. Positions on the canvas
belong to this page, not the project, so inferring does not move your layout.

As with everything here, the result changes nothing until you review and apply it.

## Where it is served, and to whom

`synqt design` binds only the loopback address, on port 8181 (`--port` changes it), and
every request carries a token created for that run. The token is in the fragment of the URL
the command prints, which a browser never sends to a server, so it never appears in a log
and is useless once you press Ctrl-C. The server refuses pages from anywhere else, by host
name and by origin, and answers no request without the token.

The site copy talks to no server. It keeps your design in this browser's storage, so the
same link opens it again on this machine and nowhere else; export it as a project to take
it elsewhere. The browser asks once when you leave a page with something drawn on it. The logo in the
corner leads back to the rest of the site.

See [build system and CLI](build-system-and-cli.md#the-synqt-command-line-tool) for the
command, [project layout and config](project-layout-and-config.md) for the file it writes,
and [getting started](getting-started.md) for the shortest path from an empty directory to
a running system. The [developer guide](development.md#adding-a-rule-to-the-designer)
covers the editor's internals, including what changes when you add a rule to it.
