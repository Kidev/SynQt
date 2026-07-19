@mainpage The C++ runtime reference

<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- @mainpage has to be the first thing in the file: anything above it, a comment
     included, makes Doxygen generate a second page for the file itself, which then shows
     up in the navigation tree next to this one. -->

@tableofcontents

This is the generated reference for %SynQt's C++ runtime, produced by Doxygen from the
headers in `src/`.

It is the reference for working on %SynQt itself, or for extending it from C++: a custom
provider, a custom entity, or embedding a runtime in an existing application. Building an
application *with* %SynQt needs none of it, because everything an application touches is
QML. Its reference is the runtime API page on <https://synqt.org/runtime-api/>.

The libraries
-------------

The runtime is split by trust boundary, one library per boundary, so that a client target
cannot link a service only module:

- **SynQtTransport**: SynQt::WebSocketTransport, the `QIODevice` over a `QWebSocket` that
  carries QtRemoteObjects. Shared by the client and the web edge.
- **SynQtClient**: SynQt::SynClient, SynQt::ServerAccessor, SynQt::Session,
  SynQt::Router, and the typed replica factory registry (SynQt::acquireReplica). Links
  into both the WebAssembly and the native desktop client.
- **SynQtConsumer**: the connect point resolver and the attached handler types behind the
  `Contract.on<Signal>` and returning slot `.then()` QML sugar.
- **SynQtService**: SynQt::EntityRuntime, SynQt::ConnectPointHost, SynQt::MeshServer,
  SynQt::MeshClient, SynQt::WebEdge, SynQt::SessionManager, SynQt::Caller,
  SynQt::IdentityProvider.
- **SynQtProviders**: SynQt::IPersistenceProvider, SynQt::IDocumentProvider,
  SynQt::ICacheProvider, SynQt::ProviderRegistry, and the bundled implementations.

What is listed
--------------

Every class and member appears, documented or not, so this is a complete map of the
runtime rather than a partial one that silently omits whatever lacks a comment.

Private members appear too, grouped separately from the callable surface. Much of what
explains a runtime class is the state it keeps rather than the state it exposes: which
QObject owns which socket, what is cached, where a lifetime ends.

Where to start
--------------

<div class="tabbed">

- <b class="tab-title">By name</b> The [class list](annotated.html) is the whole runtime,
  alphabetically, and the search box in the tab bar above resolves a partial symbol name.
- <b class="tab-title">By QML accessor</b> If you arrived from an application knowing a
  QML name rather than a class name, start at @ref qmlaccessors "QML accessors": one page
  per accessor (\qmlApp, \qmlServer, \qmlSession, \qmlRouter, \qmlCaller, \qmlClient),
  each listing every member it puts into QML and the class behind each one.
- <b class="tab-title">By boundary</b> The library list above is the trust model: pick the
  side of the boundary you are working on, then its entry point (SynQt::SynClient for a
  client, SynQt::EntityRuntime for a service, SynQt::WebEdge for an edge).

</div>

Sub-page of this one: @subpage qmlaccessors
