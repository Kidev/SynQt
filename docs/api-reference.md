<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# C++ API reference

The generated class and member reference for SynQt's C++ runtime lives at
[**/api/**](/api/index.html){ target=_blank }. It is produced by Doxygen from the
headers in `src/`, so it never drifts from the code.

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
| `SynQtClient` | `SynQt::SynClient`, `SynQt::ServerAccessor`, `SynQt::Session`, `SynQt::Router`, `SynQt::ReplicaRegistry`. |
| `SynQtConsumer` | The connect point resolver and the attached handler types behind `Contract.on<Signal>`. |
| `SynQtService` | `SynQt::EntityRuntime`, `SynQt::ConnectPointHost`, `SynQt::MeshServer`, `SynQt::MeshClient`, `SynQt::WebEdge`, `SynQt::SessionManager`, `SynQt::Caller`, `SynQt::IdentityProvider`. |
| `SynQtProviders` | `SynQt::IPersistenceProvider`, `SynQt::IDocumentProvider`, `SynQt::ICacheProvider`, `SynQt::ProviderRegistry`, and the bundled provider implementations. |

Every class and member is listed, whether or not it carries a comment, so the reference
is a complete map of the runtime rather than a partial one.

## Building it locally

The published site includes the reference: `mkdocs build` runs Doxygen through
`tools/docs-hooks/doxygen.py` and writes it into the site under `/api/`. Doxygen is
optional for a local site build; without it every other page still builds and the hook
logs that the reference was skipped.

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
