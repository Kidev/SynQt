<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Getting started

You need a terminal, a code editor, and the `synqt` command line tool.

Install `synqt`. On macOS or Linux:

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://get.synqt.org/install.ps1 | iex
```

The installer puts a single `synqt` binary on your `PATH`. Check it:

```cli
synqt version
```

> [!TIP]
> If you prefer to manage `synqt` with Python, the same CLI is on PyPI:
>
> ```cli
> pipx install synqt
> ```
>
> `pip install synqt` works too, but `synqt` is an application, which is what `pipx` is
> for. Both installs come from the same tag and behave the same.

> [!NOTE]
> `synqt` pins the rest of the toolchain (the Qt SDK, and Emscripten, which compiles
> your QML to WebAssembly) to one version per project, so every machine builds with the
> same versions. You run the install yourself: `synqt doctor` names each
> missing piece and prints the `aqt` or `emsdk` command that installs it into the
> project's `synqt/toolchain/` directory. If Qt at the pinned version is already under
> `/opt/Qt`, `~/Qt` or `QTDIR`, `synqt` uses it.

Check that your machine is ready:

```cli
synqt doctor
```

> [!TIP]
> When something will not build or run, run `synqt doctor` first. It checks your
> toolchain, ports, certificates and project topology, and usually names the problem.

## Create and run a project

```cli
synqt create
```

`synqt create` asks a few questions and scaffolds the project from your answers:

- What is the project called?
- Add authentication now? None is the default; `synqt add auth` adds it later.
- Starting entities beyond the client and edge (a database, a cache, a document
  store, a gateway, a jobs runner)? `synqt add entity` adds one later.

[`synqt new`](build-system-and-cli.md#scaffolding-a-project-synqt-new-and-synqt-create) is
the same scaffolder without the questions; use it in scripts. The name and the provider
are flags, and you add each starting entity afterwards with `synqt add entity`. `--auth`
only names the provider, so run `synqt add auth` to write the login flow:

```cli
synqt new my-app --auth github
cd my-app
synqt add auth github
synqt add entity orders --type relational
```

> [!NOTE]
> A SynQt project is a set of entities, each in its own folder. A new project has two:
> `client/` (your UI, which runs in the browser) and `web/` (the web edge, the native
> process that serves the client and faces the internet). Each thing you add, such as a
> database, a cache or a gateway, is another entity in its own folder beside them.

Run it:

```cli
cd my-app
synqt dev
```

If the toolchain is missing, `synqt dev` says so and points you to `synqt doctor`, which
prints the install commands. Run them once. Then your browser opens on the scaffolded
app, and `synqt dev` keeps watching your files and reloads the browser whenever you
save.

## Or start from one that already works

Each tutorial below ends in a project that ships with SynQt, and `synqt new` can copy it
whole. `synqt examples` lists them, and `synqt new shop --example stall` copies one. The
[quick start](quick-start.md) walks through this. Copy an example to read a finished
system, or scaffold an empty project to build your own.

## Or draw it

You can draw anything you would add with `synqt add entity` or
`synqt add connect-point`. `synqt design` opens the project as a graph in your browser:
entities are nodes and connect points are lines. The topology rules run as you work, so
a link the deployment would refuse turns red on the canvas, not in a build later. The
designer writes only after you have read the change set it offers. Drawing and typing
run the same scaffolder, so you can mix them freely. See the
[designer guide](visual-editor.md); a copy of the editor also runs
[on this site](/designer/) with nothing installed.

## Pick a tutorial

Each tutorial grows a project from nothing into a working system, one idea at a time,
and explains each idea when you first use it.

<div class="grid cards synqt-picks" markdown>

-   :material-forum: __The simple chat__

    One room, live in every window that has it open, with a moderator who can erase a
    line and a database no browser can reach. The whole model in miniature, and the
    shortest tutorial.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-chat.md)

-   :material-gavel: __The auction__

    A live auction with real time bids, sign-in through a real identity provider, and a
    Hall of Fame kept in a database. Three entities, about an hour.

    [:octicons-arrow-right-24: Start this tutorial](tutorial.md)

-   :material-gamepad-variant: __The multiplayer game__

    A shared agar.io style arena where signed-in players grow by eating, swallow smaller
    blobs, and race a ten minute round for a permanent leaderboard. Real time, with the
    server in charge, GitHub sign-in and a guest list.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-multiplayer.md)

-   :material-cube-outline: __The 3D plaza__

    A walled square in Qt Quick 3D. Every signed-in person walks around as a figure with
    their name overhead and bumps into walls, pillars and each other. Qt Quick 3D Physics
    predicts movement in the browser, and the edge decides where everybody stands.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-plaza.md)

-   :material-storefront: __The light storefront__

    A shop whose product grid ships in the bundle and whose campaign pages do not. The
    edge delivers campaign pages on demand, so a merchandiser can rewrite one without
    rebuilding the client. Three entities, a route table and a trust boundary.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-remote-pages.md)

-   :material-rocket-launch: __Shipping it__

    Take a finished project off your machine: a pipeline that refuses a bad push, the
    private authority your entities trust, where each file goes on a host, and how to
    release, roll back and sign a desktop app.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-ship.md)

</div>

To write a provider for an engine SynQt does not support, follow
[Advanced](tutorial-advanced.md): it builds a database, a cache and an identity service
against the family interfaces.

To run someone else's project, or hand yours to a reviewer, without installing a Qt SDK,
see [Running in containers](docker.md). It generates a Dockerfile and a compose file from
`synqt.yaml`, and `synqt docker up` brings the whole system up with only Docker.

The reference documentation starts at [Framework](architecture.md), which covers all of
SynQt, from the entity model to the security design.
