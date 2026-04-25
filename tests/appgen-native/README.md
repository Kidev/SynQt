<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# appgen-native: does the app generator emit code that compiles?

`tools/synqt` generates a multi-binary CMake project and one `main.cpp` per entity from a
`synqt.yaml` topology (the `synqt build` path). The generator has unit tests
(`tools/synqt/tests/test_tool.py`) that assert the content of what it emits, the right
includes, the right registrations, the right CMake wiring. Those tests cannot catch a missing
include or a CMake target collision, because a string that is absent asserts nothing.

This fixture closes that gap by building the generated code on the native host kit. Running
appgen over the real three-entity gavel topology (client + web edge + persistence database, with
connect points, a scope-gated point, identity, and a provider) and then compiling every entity is the
only check that exercises the whole service/edge/provider main path as a compiler sees it.

The kind of defect it catches, and the string tests cannot, is a root `CMakeLists.txt` that
adds `SynQtProviders` a second time and collides on the binary directory (`SynQtService`
already `PUBLIC`-links it), a service `main.cpp` that builds a `QJsonObject` with only
`<QJsonDocument>` included (which forward-declares `QJsonObject`), or an edge `main.cpp`
that upcasts the `QQmlPropertyMap*` from `EntityRuntime::accessor()` to `QObject*` for
`WebEdge::setContextObject` without including `<QQmlPropertyMap>`. Each of those is pinned
by an assertion in `test_tool.py`
(`test_service_main_includes_qjsonobject_for_the_topology`,
`test_root_cmake_guards_the_providers_subdirectory`, and the `<QQmlPropertyMap>` check in
`test_edge_main_composes_entity_runtime_for_its_mesh_side`). This fixture is the end-to-end
backstop behind those unit assertions.

## The routed client

Compiling proves a generator emits valid code. It does not prove the app works. URL routing
is the case where the two come apart. Every view a route names has to be in the client's
QML module, because the route table carries a `qrc:/qt/qml/<Uri>/<view>.qml` URL and a file
outside the module is outside the resource system. Leave one out and everything still builds,
and the router reports `pageStatus: Error` at the moment a visitor navigates.

The same is true of everything a view reaches. A `Home.qml` that instantiates a sibling
`Card.qml`, or reads a `pragma Singleton` `Theme.qml`, fails at load the same way unless
those files are in the module too, which is why the generator compiles in every `*.qml`
under the client entity's directory rather than only the views the routes name.

So the last phase runs the app. `routed/` is the smallest project that uses routing (one
client, three routes, a view that is not `Main.qml`, a view in a subdirectory, and a view
built out of a helper component and a singleton). The phase runs `synqt check` over it,
generates it, builds the client as a native desktop app, and runs it offscreen. Its
`Main.qml` is one `Loader` on `Router.pageComponent` that reports what resolved, walks the
rest of the route table reporting each time, and quits, so the run has to print:

```
SYNQT-ROUTE path=/ status=Ready view=Home(panel,dark)
SYNQT-ROUTE path=/about status=Ready view=About
SYNQT-ROUTE path=/help status=Ready view=Help
```

All `Ready`, each with the view its route names. `Home` names itself out of `Panel.qml` and
the `Theme` singleton, neither of which any route names, so that first line is the proof
that a view's own dependencies made it into the module. `/help` is `views/Help.qml`, aliased
into the module at that same relative path. A `TypeError` about `pageComponent` after
those two lines is the expected shutdown message. The accessors are torn down before the
window that binds to them, so the last binding re-evaluates against a `Router` that is
already gone.

## The promoted identity

`identity.provider_entity: auth` is documented as one line that moves the whole login engine
off the web edge and into an entity of its own. Everything that line implies is generated,
and none of it appears in any `synqt.yaml`. That is two mesh connect points (`identity` and
`sessions`), a Source QML bridge for each, an `auth/main.cpp` that builds the OAuth engine
and the authoritative session store and hands both to those Sources, and a `web/main.cpp`
that adopts the two Replicas in C++ once they initialize.

Compiling that says nothing about the claim, because the claim is about where a secret
lives. So `promoted/` is generated, given a real project mesh CA and per-entity
certificates, built, and then run. The auth entity and the edge come up as separate
processes over mutual TLS, and the phase asks the edge for a login it has no way to answer
by itself.

```
login  -> 302 https://github.com/login/oauth/authorize?client_id=Iv1.0123456789abcdef&code_challenge=...&state=...
```

Every value in that redirect (the client id, the endpoint, the PKCE challenge) came from the
auth entity over the mesh. The phase then reads both binaries and requires the edge to
contain none of the client id, the provider endpoint, or the secret, and the auth entity to
contain the first two and not the third. It reads the secret from its own environment. The
second half of that pairing is what makes the check meaningful. Without it, a generator
that dropped the provider would pass the first half, and that is a broken login rather
than a secure one.

The same fixture carries the [development sign-in](../../docs/authentication.md), because
the promotion is the arrangement it has the most to prove itself against. The server runs
inside the edge and the entity that dials it is a different process, with nothing between
them to agree through. The phase asks the edge for that login too and follows the redirect
to the stub, which must answer with the chooser naming both configured people.

```
dev login -> 302 http://127.0.0.1:8789/authorize?...&code_challenge=...&state=...
```

That is also where the `--dev` on the auth entity's own command line comes from. The
promotion moves the token exchange there, so that process is the one that decides whether
a provider entry may be spoken to. Started without it, as `synqt serve` and every
deployment start it, the edge answers this login with 403.

Two details of the phase itself guard against a green run that proves nothing. The binary
search counts matches instead of using `grep -q`. The script runs under `set -o pipefail`,
and `strings | grep -q` reports failure when grep exits early and `strings` dies of
SIGPIPE, which reads exactly like "the secret is absent". And each entity is launched with
`exec` inside its subshell, so `$!` is the process rather than a shell wrapping it and the
cleanup stops it.

## Run it

```sh
tests/appgen-native/run-appgen-native.sh
```

Needs the pinned host kit (`/opt/Qt/6.11.1/gcc_64`). It writes everything under
`build/appgen-native/` (git-ignored) and prints `APPGEN-NATIVE GATE: GO` once every generated
entity compiles and links (the `web` edge, the `database` service, and the `client`, built here
as a native desktop app), the routed client above resolves every one of its routes, and the
promoted pair signs in from the auth entity with an edge that holds no secret.

The client's WebAssembly build and the browser bring-up of a generated app are covered
separately by `synqt dev`, proven in headless Chromium. This fixture is the native half, where
the service and edge mains (which never compile in a WASM build) are exercised.
