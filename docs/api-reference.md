<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# C++ API reference

The generated class and member reference for SynQt's C++ runtime lives at
[**/api/**](api.md). It is produced by Doxygen from the
headers in [`src/`](https://github.com/Kidev/SynQt/tree/main/src), so it never drifts from the code.

This is the reference for working on SynQt itself, or for extending it from C++ (a custom
provider, a custom entity, embedding a runtime in an existing application). Building an
application with SynQt needs none of it: everything an application touches is QML, and
its reference is [runtime API](runtime-api.md).

## What is in it

Doxygen indexes the five runtime libraries. [Developer guide](development.md#the-runtime-libraries-src)
explains what each is responsible for and why they are split the way they are.

| Library | Where to start in the reference |
|---------|----------------------------------|
| `SynQtTransport` | `SynQt::WebSocketTransport`, the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. |
| `SynQtClient` | `SynQt::SynClient`, `SynQt::ServerAccessor`, `SynQt::Session`, `SynQt::Router`, and the typed replica factory registry in `replicaregistry.h`. |
| `SynQtConsumer` | The connect point resolver and the attached handler types behind `Contract.on<Signal>`. |
| `SynQtService` | `SynQt::EntityRuntime`, `SynQt::ConnectPointHost`, `SynQt::MeshServer`, `SynQt::MeshClient`, `SynQt::WebEdge`, `SynQt::SessionManager`, `SynQt::Caller`, `SynQt::IdentityProvider`. |
| `SynQtProviders` | `SynQt::IPersistenceProvider`, `SynQt::IDocumentProvider`, `SynQt::ICacheProvider`, `SynQt::ProviderRegistry`, and the bundled provider implementations. |

Every class and member is listed, whether or not it carries a comment, so the reference
is a complete map of the runtime rather than a partial one. Private members are listed
too, grouped separately from the callable surface: much of what explains a runtime class
is the state it keeps, not the state it exposes.

### Finding a class from a QML name

An application knows `Server` and `Caller`, not `ServerAccessor` and the class that binds
`Client`. The [QML accessors](api.md?p=qmlaccessors.html) section of the
reference bridges the two, with a page per accessor:
[App](api.md?p=qmlapp.html),
[Server](api.md?p=qmlserver.html),
[Session](api.md?p=qmlsession.html),
[Router](api.md?p=qmlrouter.html),
[Caller](api.md?p=qmlcaller.html), and
[Client](api.md?p=qmlclient.html). Each says what the name is, which class
implements it, and which side of the trust boundary it links into. The members themselves
are on [runtime API](runtime-api.md), written for the QML that calls them.

## Building it locally

The published site includes the reference: `mkdocs build` runs Doxygen through
[`tools/docs-hooks/doxygen.py`](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen.py) and writes
it into the site under `/api/ref/`, which [`/api/`](api.md) shows in a frame. Doxygen is
optional for a local site build; without it every other page still builds and the hook
logs that the reference was skipped.

`/api/` is an ordinary page of this site ([`docs/api.md`](https://github.com/Kidev/SynQt/blob/main/docs/api.md)
with [`overrides/api.html`](https://github.com/Kidev/SynQt/blob/main/overrides/api.html)),
so the header, the tabs, the search and the Download button around the reference are the
site's own rather than a copy of them, and they are drawn once for a whole visit through
the reference instead of on every page. The address bar follows the page you are on
(`/api/?p=classSynQt_1_1WebEdge.html`), and a generated page opened on its own redirects
into that shell, so links into the reference keep working wherever they come from.

Everything else lives in [`Doxyfile`](https://github.com/Kidev/SynQt/blob/main/Doxyfile) at the repository root:
the input set, the Qt macro handling, and the theme. The pages are styled with
[doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) (MIT), vendored
under [`tools/docs-hooks/doxygen-awesome/`](https://github.com/Kidev/SynQt/tree/main/tools/docs-hooks/doxygen-awesome)
so a docs build needs no network, in its sidebar layout: the class tree down the left is
the primary navigation and carries the search box, with the page outline on the right. On
top of it sit a
[SynQt brand layer](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-synqt.css),
a [custom header](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-header.html)
that joins each page to the shell page above, and a
[custom footer](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen-footer.html)
carrying the license instead of a generator credit.

The two navigation panels each have one job, which the hook enforces after Doxygen runs.
The tree on the left lists pages and only pages: Doxygen also files a class's member
sections there, which are anchors in the page that class already occupies, so entries side
by side in the tree meant two different things and the same content appeared in both
panels. The outline on the right lists the sections of the page you are on, which is where
those members are now. The hook also stops the tree from remembering a selection: Doxygen
caches the last entry clicked and reselects it on every later page, which left the
highlight stuck on whatever was opened first.

To generate it on its own, into `build/apidocs/html/index.html`:

```sh
doxygen Doxyfile
```

Install Doxygen (and Graphviz, for the inheritance diagrams) from your package manager:
`apt install doxygen graphviz`, `brew install doxygen graphviz`, or
`pacman -S doxygen graphviz`.

## Documenting new code

Doxygen reads `///` comment blocks placed directly above the declaration they describe.
The first sentence becomes the brief shown in the class listing, so lead with what the
thing is:

```cpp
/// Carries QtRemoteObjects over a QWebSocket. QtRO speaks QIODevice and QWebSocket does
/// not, so every message is moved through this adapter: outgoing writes become binary
/// frames, incoming frames land in a read buffer.
class WebSocketTransport : public QIODevice
{
```

A plain `//` comment is invisible to Doxygen and stays a note to the next reader, which
is the right choice for a remark about one line of implementation. Use `///` for anything
that describes a class, a member, or an argument, so it reaches the reference.
