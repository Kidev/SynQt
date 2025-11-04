<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The visual editor

A SynQt system is a handful of entities and the connect points between them, which is a
drawing before it is a configuration file. The editor is that drawing, live: entities as
nodes, connect points as the lines between them, and a panel for what each one carries.

There are two of it, and they are the same page.

- **In a project.** `synqt design` serves it on this machine and opens it. What you draw
  is that project: Apply writes `synqt.yaml`, the contracts under `shared/`, and the QML
  files a new entity or connect point needs.
- **[On this site](/designer/).** The same editor with nothing behind it. Draw a system,
  press Download, and you get the project as a zip. Nothing is installed and nothing is
  read off your machine, because there is no machine on the other end of the page.

The second is the one to open first if you have not installed anything yet. Unzip what it
gives you over a project made with `synqt new`, or keep it as a sketch and open the real
thing later.

## What you can draw

The rail on the left is the entity palette, and it is the list from
[entities](entities.md): a client, a web edge, the four blueprints that come with an engine
behind them (persistence, cache, document, gateway), the jobs blueprint, and a plain service
you write yourself. Adding one drops it in the column it belongs in, because the layout says
something: the browser on the left, the edge it reaches in the middle, and everything it
must not reach on the right.

A connect point is drawn from the entity that **owns** it to the one that **consumes** it.
Drag the handle on the owner's rim and drop the line on the consumer. That direction is the
whole meaning of the line, so it is the thing the canvas asks you to say first.

Selecting a node or a line opens the panel on the right, which is where the rest lives: an
entity's blueprint and provider, a connect point's name and contract, its consumer list, and
what crosses it. The consumer list is the authorization, not a hint; an entity that is not
on it is refused the replica. [Security](security.md) is where that is spelled out.

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
what you came to fix, so the door is not the gate; the verdict arrives with the project,
painted on the canvas, and it is Apply that holds the line.

## Reading the contracts back

"Infer from the sources" is [`synqt infer`](build-system-and-cli.md#the-synqt-command-line-tool)
on the canvas. It reads the project's own QML, both ends of every link, and fills each
connect point with the members the code already uses: the props the owner's Source assigns,
the models it pushes, the signals it emits, and the slots the consumers call. A contract
you have never written arrives drawn instead of typed out.

It is evidence, not proof, and the page says so. A member nothing in the QML gave a type
to comes back `var`, the hint counts them, and each one is yours to open and name. Where a
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
close the tab and the drawing is gone. Download it first.

See [build system and CLI](build-system-and-cli.md#the-synqt-command-line-tool) for the
command, [project layout and config](project-layout-and-config.md) for what the file it
writes means, and [getting started](getting-started.md) for the shortest path from an empty
directory to something running. The [developer guide](development.md#adding-a-rule-to-the-visual-editor)
covers the editor from the other side, including what moves when a rule is added to it.
