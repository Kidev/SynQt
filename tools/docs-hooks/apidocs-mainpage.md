@mainpage The C++ runtime reference

<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- @mainpage has to be the first thing in the file. Anything above it, a comment
     included, makes Doxygen generate a second page for the file itself, which then shows
     up in the navigation tree next to this one. -->

@tableofcontents

This is the generated reference for %SynQt's C++ runtime, produced by Doxygen from the
headers in `src/`.

It is the reference for working on %SynQt itself, or for extending it from C++ with a custom
provider, a custom entity, or a runtime embedded in an existing application. Building an
application with %SynQt needs none of it, because everything an application touches is
QML. Its reference is the runtime API page on <https://synqt.org/runtime-api/>.

The libraries
-------------

The runtime is split by trust boundary, one library per boundary, so that a client target
cannot link a service only module:

- SynQtTransport holds SynQt::WebSocketTransport, the `QIODevice` over a `QWebSocket` that
  carries QtRemoteObjects. The client and the web edge share it.
- SynQtClient holds SynQt::SynClient, SynQt::ServerAccessor, SynQt::Session,
  SynQt::Router, and the typed replica factory registry (SynQt::acquireReplica). It links
  into both the WebAssembly and the native desktop client.
- SynQtConsumer holds the connect point resolver and the attached handler types behind the
  `<Owner>.on<Signal>` and returning slot `.then()` QML sugar.
- SynQtService holds SynQt::EntityRuntime, SynQt::ConnectPointHost, SynQt::MeshServer,
  SynQt::MeshClient, SynQt::SessionManager, SynQt::Caller. Every module it links is
  LGPLv3, which is what keeps a plain service entity LGPLv3.
- SynQtIdentity holds SynQt::OAuthBackend, SynQt::JwksVerifier, SynQt::IdentityService. Qt
  Network Authorization is GPLv3-only, so it is a library of its own.
- SynQtEdge holds SynQt::WebEdge, SynQt::IdentityProvider, SynQt::PagesService. Qt HTTP
  Server is GPLv3-only, so only a `type: web_edge` entity links it.
- SynQtGateway holds SynQt::ApiServer and the `Api` helper an entity's `network.inbound`
  opens.
- SynQtProviders holds SynQt::IPersistenceProvider, SynQt::IDocumentProvider,
  SynQt::ICacheProvider, SynQt::ProviderRegistry, and the bundled implementations.
- SynQtMonitor holds SynQt::MonitorService, SynQt::EventStore and the two exporters. It is
  SynQtEdge plus a history, so it is GPLv3 like the edge.
- SynQtContract holds SynQt::SourceModel, the model a generated Source publishes its rows
  through, which a consumer cannot write into.
- SynQtTesting holds SynQt::EntityTest, the harness behind `synqt test`. A production
  entity never links it.

What is listed
--------------

Every class and member appears, documented or not, so this is a complete map of the
runtime rather than a partial one that silently omits whatever lacks a comment.

Private members appear too, grouped separately from the callable surface. Much of what
explains a runtime class is the state it keeps rather than the state it exposes, which
QObject owns which socket, what is cached, where a lifetime ends.

Where to start
--------------

<div class="tabbed">

- <b class="tab-title">By name</b> The [class list](annotated.html) is the whole runtime,
  alphabetically, and the search box in the tab bar above resolves a partial symbol name.
- <b class="tab-title">By QML accessor</b> If you arrived from an application knowing a
  QML name rather than a class name, start at @ref qmlaccessors "QML accessors", one page
  per accessor (\qmlApp, \qmlServer, \qmlSession, \qmlRouter, \qmlCaller, \qmlClient),
  each listing every member it puts into QML and the class behind each one.
- <b class="tab-title">By boundary</b> The library list above is the trust model. Pick the
  side of the boundary you are working on, then its entry point (SynQt::SynClient for a
  client, SynQt::EntityRuntime for a service, SynQt::WebEdge for an edge).

</div>

Sub-page of this one: @subpage qmlaccessors
