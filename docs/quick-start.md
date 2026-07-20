<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Quick start

Install the CLI, copy an example system, and run it. After the install, two commands
bring up a real SynQt system: a client compiled to WebAssembly in your browser, a web
edge serving it, and a database the browser cannot reach. The toolchain takes a few
minutes to install the first time; everything else is quick.

You write no code on this page. [Getting started](getting-started.md) builds a project
of your own.

## Install the CLI

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
```

On Windows, in PowerShell, it is `irm https://get.synqt.org/install.ps1 | iex`. If you
already have Python, `pipx install synqt` gets you the same CLI from PyPI.

```cli
synqt version
```

The rest of the toolchain is the Qt SDK and Emscripten, the compiler that turns your QML
into WebAssembly. `synqt` pins both to one version per project, so every machine builds
with the same versions. `synqt doctor` prints the exact `aqt` and `emsdk` commands that
install whatever is missing into the project's `synqt/toolchain/` directory:

```cli
synqt doctor
```

Run the commands it prints, once. If Qt at the pinned version is already installed under
`/opt/Qt`, `~/Qt` or `QTDIR`, `synqt` uses it. To install nothing at all, use
[`synqt docker up`](docker.md), which builds and runs the whole system in containers.

## Copy an example

```cli
synqt examples
```

```text
  arena  the multiplayer tutorial, materialized
  chat   a room everybody in it sees at once
  gavel  the auction tutorial, materialized
  plaza  the 3D tutorial, materialized
  stall  a storefront with edge-delivered campaigns

Start one with: synqt new <directory> --example <name>
```

Take the storefront: it runs with no setup. The other examples sign people in, which
first needs an OAuth app you would have to register.

```cli
synqt new shop --example stall
cd shop
```

This is a complete project, with the `synqt.yaml`, entity folders and QML you would
write by hand, and `project.name` set to the directory you named. Its `README.md`
explains what the example does.

## Run it

```cli
synqt dev
```

`synqt dev` runs the whole development loop:

1. **Checks** `synqt.yaml` with `synqt check`, and stops if the topology fails.
2. **Issues certificates:** a throwaway development certificate authority and one
   certificate per entity, so the mesh links run over mutual TLS from the start.
3. **Builds** every entity: the client to WebAssembly, the rest as native binaries.
4. **Starts** them, owners before consumers and the edge last, and opens
   [http://127.0.0.1:8080](http://127.0.0.1:8080).

The first run compiles everything from scratch and takes a few minutes. Later runs take
seconds.

## What you are looking at

The storefront in the browser is a Qt Quick application compiled to WebAssembly, holding a
live [QtRemoteObjects](https://doc.qt.io/qt-6/qtremoteobjects-index.html) link to the edge
over a WebSocket. Three entities are running:

| Entity | What it is | Who can reach it |
| --- | --- | --- |
| `app` | the client, in your browser | you |
| `edge` | the web edge: serves the bundle, owns the live catalog, delivers the campaign pages | the browser, over wss |
| `stock` | the database holding the durable stock | the edge, over mutual TLS |

The browser talks only to the edge. It holds no certificate and no route into the mesh,
so it cannot address the database at all. Every SynQt system has this shape; the
[entity model](entities.md) sets it, not a deployment choice.

Leave `synqt dev` running. It watches every `.qml` file and `synqt.yaml`, rebuilds what a
save affects, and reloads the browser. Open `client/app/Home.qml`, change a label, save,
and the page reloads with the change.

## Or draw it first

To see the shape of a system before running one, open the [designer](/designer/). Draw
the entities and the links between them, press Export to take it as a project, and unzip
the result over a project made with `synqt new`. It installs nothing and nothing leaves
the page. The [designer guide](visual-editor.md) covers what it can do, and **Examples**
in its toolbar opens each system above on the canvas.

## Where to go next

- [Getting started](getting-started.md) builds a project of your own from an empty
  scaffold, and covers `synqt create`, which asks the scaffold's questions instead of
  taking defaults.
- [The simple chat](tutorial-chat.md) is the shortest tutorial that ends in a real
  application: one room, sign-in, and a column the browser never receives.
- [The auction tutorial](tutorial.md) builds a system end to end: live bidding, then
  sign-in, then a database the browser cannot reach. It ends at the `gavel` example.
- [Architecture](architecture.md) is the reference, from the entity model to the
  security design.
