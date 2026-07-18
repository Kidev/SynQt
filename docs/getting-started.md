# Getting started

You need three things: a terminal, a code editor, and the `synqt` command line
tool.

Install `synqt`. On macOS or Linux:

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
```

On Windows, in PowerShell:

```powershell
irm https://get.synqt.org/install.ps1 | iex
```

The installer drops a single `synqt` binary on your `PATH`. Confirm it is there:

```cli
synqt --version
```

> [!NOTE]
> That one binary is all you install by hand. The first time you build a project,
> `synqt` downloads and pins the rest of the toolchain it needs (the Qt SDK and the
> Emscripten compiler that turns your QML into WebAssembly) into the project, so
> every machine and every teammate gets the exact same versions. You never install
> Qt or Emscripten yourself.

Now check your machine is ready:

```cli
synqt doctor
```

> [!TIP]
> `synqt doctor` is your friend throughout. Whenever something will not build or
> run, run it first. It checks your toolchain, ports, certificates, and project
> topology and usually tells you exactly what is wrong.

## Create and run a project

```cli
synqt new my-app
```

`synqt new` asks a few questions as it scaffolds the project:

- Same origin for the client and the web edge, or split across two origins? Same
  origin is the simplest and safest setup, and the recommended default.
- Add authentication now? It can always be added later with `synqt add auth`.
- Starting entities beyond the client and edge (a database, a cache, a gateway)?
  These can also be added later with `synqt add entity`.

> [!NOTE]
> A SynQt project is a set of entities, each in its own folder. A new project
> always has two: `client/` (your UI, which runs in the browser) and `web/` (the
> web edge, the native process that serves the client and faces the internet).
> Anything else you add, a database, a cache, a gateway, is its own entity and its
> own folder alongside them.

Run it:

```cli
cd my-app
synqt dev
```

The first run installs the toolchain, so it takes a few minutes. After that your
browser opens to the scaffolded app, and `synqt dev` keeps watching your files,
reloading the browser whenever you save.

## Pick a tutorial

Each tutorial grows this project from nothing into a working system, one idea at
a time, and each idea is explained as you use it rather than up front.

<div class="grid cards" markdown>

-   :material-gavel: __The auction__

    A live auction with real time bids, sign in through a real identity provider,
    and a persistent Hall of Fame backed by a database. Three entities, about an
    hour.

    [:octicons-arrow-right-24: Start this tutorial](tutorial.md)

-   :material-gamepad-variant: __The multiplayer game__

    A shared agar.io-style arena where signed in players grow by eating, swallow
    smaller blobs, and race a ten minute round for a permanent leaderboard. Real
    time, server-authoritative, with GitHub sign in and a guest list.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-multiplayer.md)

</div>

Prefer the reference documentation instead? [Framework](architecture.md) covers
every part of SynQt, from the entity model down to the security design.
