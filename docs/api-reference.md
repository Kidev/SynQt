<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# C++ API reference

The class and member reference for SynQt's C++ runtime is at [/api/](api.md). Doxygen
generates it from the headers in [`src/`](https://github.com/Kidev/SynQt/tree/main/src), so
it always matches the code.

Use it to work on SynQt itself, or to extend it from C++ (a custom provider, a custom
entity, a runtime embedded in an existing application). Building an application needs none
of it: an application only touches QML, documented in [runtime API](runtime-api.md).

## What is in it

Doxygen indexes the runtime libraries. The
[developer guide](development.md#the-runtime-libraries-src) explains what each one does and
why they are split this way.

| Library | Where to start in the reference |
|---------|----------------------------------|
| `SynQtTransport` | `SynQt::WebSocketTransport`, the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. |
| `SynQtClient` | `SynQt::SynClient`, `SynQt::ServerAccessor`, `SynQt::Session`, `SynQt::Router`, and the typed replica factory registry in `replicaregistry.h`. |
| `SynQtConsumer` | The connect point resolver and the attached handler types behind `<Owner>.on<Signal>`. |
| `SynQtService` | `SynQt::EntityRuntime`, `SynQt::ConnectPointHost`, `SynQt::MeshServer`, `SynQt::MeshClient`, `SynQt::SessionManager`, `SynQt::Caller`. |
| `SynQtIdentity` | `SynQt::OAuthBackend`, `SynQt::JwksVerifier`, `SynQt::IdentityService`. |
| `SynQtEdge` | `SynQt::WebEdge`, `SynQt::IdentityProvider`, `SynQt::PagesService`, `SynQt::PageStore`. |
| `SynQtGateway` | `SynQt::ApiServer` and the `Api` helper an entity's `network.inbound` opens. |
| `SynQtProviders` | `SynQt::IPersistenceProvider`, `SynQt::IDocumentProvider`, `SynQt::ICacheProvider`, `SynQt::ProviderRegistry`, and the bundled provider implementations. |
| `SynQtMonitor` | `SynQt::MonitorService`, `SynQt::EventStore`, and the two exporters, `SynQt::OtlpExporter` and `SynQt::JsonlExporter`. |
| `SynQtContract` | `SynQt::SourceModel`, the model a generated Source publishes its rows through. |
| `SynQtTesting` | `SynQt::EntityTest`, the harness behind `synqt test`. |

Every class and member is listed, commented or not, so the reference maps the whole
runtime. Private members are listed too, grouped apart from the public API, because a
class's internal state often explains it best.

### Finding a class from a QML name

An application knows `Server` and `Caller`, not `ServerAccessor` or the class behind
`Client`. The reference's [QML accessors](api.md?p=qmlaccessors.html) section links the
two, with one page per accessor:
[App](api.md?p=qmlapp.html),
[Server](api.md?p=qmlserver.html),
[Session](api.md?p=qmlsession.html),
[Router](api.md?p=qmlrouter.html),
[Caller](api.md?p=qmlcaller.html), and
[Client](api.md?p=qmlclient.html). Each says what the name is, which class implements it,
and which side of the trust boundary links it. [Runtime API](runtime-api.md) documents the
members for the QML that calls them.

## Building it locally

The published site includes the reference. `mkdocs build` runs Doxygen through
[`tools/docs-hooks/doxygen.py`](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen.py)
and writes it under `/api/ref/`, which [`/api/`](api.md) shows in a frame. A local site
build works without Doxygen: every other page builds, and the hook logs that it
skipped the reference.

`/api/` is an ordinary page of this site
([`docs/api.md`](https://github.com/Kidev/SynQt/blob/main/docs/api.md) with
[`overrides/api.html`](https://github.com/Kidev/SynQt/blob/main/overrides/api.html)), so the
header, tabs, search and Download button around the reference are the site's own, drawn
once per visit instead of on every page. The address bar follows the current page
(`/api/?p=classSynQt_1_1WebEdge.html`), and a generated page opened directly redirects into
that shell, so links into the reference work from anywhere.

Everything else is in [`Doxyfile`](https://github.com/Kidev/SynQt/blob/main/Doxyfile) at
the repository root: the input files, the Qt macro handling and the theme. The pages use
[doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) (MIT), vendored under
[`tools/docs-hooks/doxygen-awesome/`](https://github.com/Kidev/SynQt/tree/main/tools/docs-hooks/doxygen-awesome)
so a docs build needs no network. The sidebar layout puts the class tree and the search box
on the left, and the page outline on the right. On top of it sit a
[SynQt brand layer](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-synqt.css),
a [custom header](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-header.html)
that joins each page to the shell page above, and a
[custom footer](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-footer.html)
carrying the license instead of a generator credit.

The brand layer carries this site's type sizes as well as its colors, so a reference page
uses the same sizes as the rest of the site. The reference also loads the site's
[scrollbar stylesheet](https://github.com/Kidev/SynQt/blob/main/docs/stylesheets/scrollbar.css)
and [script](https://github.com/Kidev/SynQt/blob/main/docs/javascripts/scrollbar.js), like
every other page. That puts the scrollbar at the window's right edge: on a reference page
the middle of three panels scrolls, so the native bar would sit three hundred pixels short
of the edge.

The hook gives each navigation panel one job after Doxygen runs:

- **The left tree lists pages only.** Doxygen would also list a class's member sections
  there, which are anchors within the class's page, so the same content would appear in
  both panels.
- **The right outline lists the current page's sections,** members included.

The hook also stops the tree from remembering a selection: Doxygen caches the last entry
clicked and reselects it on every later page, which leaves the highlight stuck.

To generate it on its own, into `build/apidocs/html/index.html`:

```sh
doxygen Doxyfile
```

Install Doxygen (and Graphviz, for the inheritance diagrams) from your package manager:
`apt install doxygen graphviz`, `brew install doxygen graphviz`, or
`pacman -S doxygen graphviz`.

## Documenting new code

Doxygen reads `///` comment blocks placed directly above the declaration they describe.
The first sentence becomes the brief in the class listing, so start with what the thing
is:

```cpp
/// Carries QtRemoteObjects over a QWebSocket. QtRO speaks QIODevice and QWebSocket does
/// not, so every message is moved through this adapter: outgoing writes become binary
/// frames, incoming frames land in a read buffer.
class WebSocketTransport : public QIODevice
{
```

Doxygen ignores a plain `//` comment, which suits a note about one line of
implementation. Use `///` for anything that describes a class, a member or an argument, so
it appears in the reference.
