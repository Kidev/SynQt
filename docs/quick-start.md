<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Quick start

Install the CLI, scaffold a project, and you have a client running in your browser against
a real edge. Then add one slot and a test that proves who may call it. The first build
downloads a toolchain and takes a few minutes; nothing else here is slow.

## Install the CLI

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
```

On Windows, in PowerShell, it is `irm https://get.synqt.org/install.ps1 | iex`. If you
already have Python, `pipx install synqt` gets you the same CLI from PyPI.

```cli
synqt --version
```

That single binary is everything you install by hand. The first build downloads the rest of
the toolchain, the Qt SDK and the Emscripten compiler that turns your QML into WebAssembly,
and pins both inside the project, so every machine and every teammate compiles against the
same versions.

## Get the server running

```cli
synqt new app
cd app
synqt dev
```

`synqt dev` is the whole development loop in one command. It issues a throwaway development
certificate authority and a certificate per entity, so the mesh links between your services
run over mutual TLS from the first second rather than from the day you remember to turn it
on. It runs `synqt check` over `synqt.yaml` first and refuses to start on a topology that
does not pass.
It builds every entity, the client to WebAssembly and the edge as a native binary. Then it
starts them, owners before consumers and the edge last, and opens
[http://127.0.0.1:8080](http://127.0.0.1:8080).

The first run takes a few minutes because of the toolchain download. Every run after that
is seconds.

The page says "Connecting..." and then "Connected". That is the client, compiled to
WebAssembly, holding a live QtRemoteObjects link to the edge over a WebSocket.

Leave it running. It watches every `.qml` file and `synqt.yaml`, rebuilds what a save
actually affects, and reloads the browser for you.

## Give it something worth testing

A new project has two entities, `client/app/` and `web/edge/`, and no connect point between
them. A connect point is the only way anything crosses, so until there is one there is
nothing to test. Add one, owned by the edge, consumed by the client:

```cli
synqt add connect-point edge --consumers app
```

That writes a starter `export:` block on the point in `synqt.yaml` and turns
`web/edge/Edge.qml` into the Source that answers it. Replace the starter block with two
members:

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    export: |
      prop int count      // the owner writes it, consumers read it
      slot bump()         // a consumer asks; the owner decides
```

Then write the slot in `web/edge/Edge.qml`:

```qml
import SynQt

Edge {
    id: root

    count: 0

    function bump() {
        // The gate. `Caller` is whoever made this call, and on a point a browser
        // consumes that is a person with a session and a scope.
        if (!Caller.hasScope("user")) {
            return;
        }
        root.count += 1;
    }
}
```

Nothing in the client can raise its own scope, and nothing undeclared crosses the point, so
this one function is where the rule lives.

## Run a test against it

Tests are QML, they live in `tests/`, and they are named `tst_<Something>.qml`. Write
`tests/tst_Edge.qml`:

```qml
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    id: suite

    name: "Edge"

    EntityTest {
        id: harness

        source: "../web/edge/Edge.qml"
    }

    // A fresh Source per function, so nothing passes because of the order it ran in.
    function init() {
        verify(harness.load(), harness.errorString);
    }

    function test_a_signed_out_visitor_cannot_bump() {
        harness.callerIsUser("anonymous");
        harness.subject.bump();
        compare(harness.subject.count, 0);
    }

    function test_a_signed_in_user_can() {
        harness.callerIsUser("user");
        harness.subject.bump();
        compare(harness.subject.count, 1);
    }
}
```

Save it, and in a second terminal:

```text
$ synqt test
Test project /home/you/app/build/host
    Start 1: app-tests
1/1 Test #1: app-tests ........................   Passed    0.09 sec

100% tests passed out of 1
```

`synqt test` builds the test target and runs it under CTest. Every test file after the
first is picked up with no rebuild at all, because Qt Quick Test finds `tests/tst_*.qml`
by directory when the target runs.

There is no browser in that run, no database to start, and no certificate to issue. The
harness loads the real Source and mints a real `Caller` through the same factory the
transports use, so the check the test exercises is the check a deployment runs. There is no
test-only door into it, which is the point: a gate you can only reach through a browser is
a gate nobody tests.

## Or draw it first

If you would rather see the shape of a system before typing anything, open the
[designer](/designer/). Draw the entities and the links between them, press Download, and
unzip the result over a project made with `synqt new`. Nothing is installed and nothing
leaves the page. The [guide to the designer](visual-editor.md) covers what it can do.

## Where to go next

- [Getting started](getting-started.md) walks the same ground slowly, and covers
  `synqt create`, which asks the security relevant questions instead of taking defaults.
- [The auction tutorial](tutorial.md) builds a real system end to end: live bidding, then
  sign-in, then a database the browser cannot reach.
- [Testing your app](testing.md) covers the rest of the harness: entity callers, the
  in-memory engines behind `Db` and `Cache`, and asserting on what an entity said.
- [Architecture](architecture.md) is the reference, from the entity model to the security
  design.
