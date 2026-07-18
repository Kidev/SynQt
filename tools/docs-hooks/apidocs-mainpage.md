<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

SynQt C++ runtime reference {#mainpage}
===========================

This is the generated reference for %SynQt's C++ runtime, produced by Doxygen from the
headers in `src/`.

It is the reference for working on %SynQt itself, or for extending it from C++: a custom
provider, a custom entity, or embedding a runtime in an existing application. Building an
application *with* %SynQt needs none of it, because everything an application touches is
QML. Its reference is the runtime API page on <https://synqt.org/>.

The runtime is split by trust boundary, one library per boundary, so that a client target
cannot link a service only module:

- **SynQtTransport**: SynQt::WebSocketTransport, the `QIODevice` over a `QWebSocket` that
  carries QtRemoteObjects. Shared by the client and the web edge.
- **SynQtClient**: SynQt::SynClient, SynQt::ServerAccessor, SynQt::Session,
  SynQt::Router, and the typed replica factory registry (SynQt::acquireReplica). Links
  into both the WebAssembly and the native
  desktop client.
- **SynQtConsumer**: the connect point resolver and the attached handler types behind the
  `Contract.on<Signal>` and returning slot `.then()` QML sugar.
- **SynQtService**: SynQt::EntityRuntime, SynQt::ConnectPointHost, SynQt::MeshServer,
  SynQt::MeshClient, SynQt::WebEdge, SynQt::SessionManager, SynQt::Caller,
  SynQt::IdentityProvider.
- **SynQtProviders**: SynQt::IPersistenceProvider, SynQt::IDocumentProvider,
  SynQt::ICacheProvider, SynQt::ProviderRegistry, and the bundled implementations.

Every class and member is listed, whether or not it carries a comment, so this is a
complete map of the runtime rather than a partial one.

Start from the [class list](annotated.html), or use the search box.
