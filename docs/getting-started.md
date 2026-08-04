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

> [!TIP]
> If you already have Python and would rather manage `synqt` with it, the same CLI is on
> PyPI:
>
> ```cli
> pipx install synqt
> ```
>
> `pip install synqt` works too; `pipx` is the suggestion only because this is an
> application rather than a library. Both installs are cut from the same tag and behave
> identically. The rest of this page reads the same either way.

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
synqt create
```

`synqt create` asks a short, security relevant set of questions and scaffolds the
project from the answers:

- What is the project called?
- Add authentication now? None is the default; `synqt add auth` adds it later.
- Starting entities beyond the client and edge (a database, a cache, a document
  store, a gateway, a jobs runner)? `synqt add entity` adds one later.

Every one of those is also a flag on [`synqt new`](build-system-and-cli.md#scaffolding-a-project-synqt-new-and-synqt-create),
which is the same scaffolder without the questions and the one to use in a script:

```cli
synqt new my-app --auth github --blueprint persistence
```

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

<div class="grid cards synqt-picks" markdown>

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

-   :material-storefront: __The light storefront__

    A shop whose product grid ships in the bundle and whose campaign pages do not:
    the edge delivers those on demand, so a merchandiser can rewrite one without
    rebuilding a client. Two entities, a route table, and a trust boundary.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-remote-pages.md)

-   :material-rocket-launch: __Shipping it__

    Take a finished project off your machine: a pipeline that refuses a bad push, the
    private authority your entities trust, where every file goes on a host, and what a
    release, a rollback and a signed desktop app actually involve.

    [:octicons-arrow-right-24: Start this tutorial](tutorial-ship.md)

</div>

Writing a provider for an engine SynQt has no support for is its own track:
[Advanced](tutorial-advanced.md) builds a database, a cache, and an identity service
against the family interfaces.

Want to run somebody else's project, or hand yours to a reviewer, without either of you
installing a Qt SDK? [Running in containers](docker.md) generates a Dockerfile and a
compose file from `synqt.yaml`, so `synqt docker up` brings the whole system up with
Docker and nothing else.

Prefer the reference documentation instead? [Framework](architecture.md) covers
every part of SynQt, from the entity model down to the security design.
