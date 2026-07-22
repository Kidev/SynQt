<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# appgen-native: does the app generator emit code that actually compiles?

`tools/synqt` generates a multi-binary CMake project and one `main.cpp` per entity from a
`synqt.yaml` topology (the `synqt build` path). The generator has unit tests
(`tools/synqt/tests/test_tool.py`) that assert the *content* of what it emits; the right
includes, the right registrations, the right CMake wiring. Those tests cannot catch a **missing**
include or a CMake target collision, because a string that is absent asserts nothing.

This fixture closes that gap by **building** the generated code on the native host kit. Running
appgen over the real three-entity gavel topology (client + web edge + persistence database, with
connect points, `per_session`, identity, and a provider) and then compiling every entity is the
only check that exercises the whole service/edge/provider main path as a compiler sees it.

It earned its place: the first time it ran it found three defects the string tests had missed;

1. the generated root `CMakeLists.txt` added `SynQtProviders` a second time, colliding on the
   binary directory (`SynQtService` already `PUBLIC`-links it), so configuration failed for any
   project with a blueprint/provider entity;
2. the service `main.cpp` built a `QJsonObject` from the topology with only `<QJsonDocument>`
   included (which forward-declares `QJsonObject`), so no service entity compiled;
3. the edge `main.cpp` upcast the `QQmlPropertyMap*` from `EntityRuntime::accessor()` to
   `QObject*` for `WebEdge::setContextObject` without including `<QQmlPropertyMap>`, so no
   mesh-consuming edge compiled.

All three are fixed in `appgen.py` and pinned by new assertions in `test_tool.py`
(`test_service_main_includes_qjsonobject_for_the_topology`,
`test_root_cmake_guards_the_providers_subdirectory`, and the `<QQmlPropertyMap>` check in
`test_edge_main_composes_entity_runtime_for_its_mesh_side`). This fixture is the end-to-end
backstop behind those unit assertions.

## The routed client

Compiling proves a generator emits valid code; it does not prove the app works. URL routing
is the case where the two come apart: every view a route names has to be **in** the client's
QML module, because the route table carries a `qrc:/qt/qml/<Uri>/<view>.qml` URL and a file
outside the module is outside the resource system. Leave one out and everything still builds,
and the router reports `pageStatus: Error` at the moment a visitor navigates.

So the last phase runs the app. `routed/` is the smallest project that uses routing (one
client, two routes, and a view that is not `Main.qml`); the phase runs `synqt check` over it,
generates it, builds the client as a native desktop app, and runs it offscreen. Its `Main.qml`
is one `Loader` on `Router.pageComponent` that reports what resolved, walks to the second
route, reports again, and quits, so the run has to print:

```
SYNQT-ROUTE path=/ status=Ready view=Home
SYNQT-ROUTE path=/about status=Ready view=About
```

Both `Ready`, each with the view its route names. A `TypeError` about `pageComponent` after
those two lines is the expected shutdown message: the accessors are torn down before the
window that binds to them, so the last binding re-evaluates against a `Router` that is
already gone.

## Run it

```sh
tests/appgen-native/run-appgen-native.sh
```

Needs the pinned host kit (`/opt/Qt/6.11.1/gcc_64`). It writes everything under
`build/appgen-native/` (git-ignored) and prints `APPGEN-NATIVE GATE: GO` when every generated
entity; the `web` edge, the `database` service, and the `client` (built here as a native desktop
app); compiles and links, and the routed client above resolves both of its routes.

The client's WebAssembly build and the browser bring-up of a generated app are covered separately
by `synqt dev` (proven in headless Chromium; see the M10/TOOL-1 work); this fixture is the native
half, where the service and edge mains (which never compile in a WASM build) are exercised.
