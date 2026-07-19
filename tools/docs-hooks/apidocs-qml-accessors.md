@page qmlaccessors QML accessors

<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

Most of this runtime exists to put six names into QML. An application never constructs
the C++ classes below; it writes `Server.chat.send(text)` or `Caller.hasScope("admin")`
and the runtime binds the object behind the name. The name and the class are not the
same thing, and searching this reference for `Server` finds a class that is not called
`Server`, so each accessor gets a page of its own here: what the name is, which class
implements it, and where the full member list lives.

The authoritative reference for the members themselves is the runtime API page on
<https://synqt.org/runtime-api/>, written for the QML that calls them. These pages are
the bridge from that name to this C++.

- @subpage qmlapp
- @subpage qmlserver
- @subpage qmlsession
- @subpage qmlrouter
- @subpage qmlcaller
- @subpage qmlclient

@page qmlapp App

`App` is the running client itself, as opposed to \ref qmlserver "Server", which is
everything the client reaches over the wire. It carries the client update surface:
`App.onUpdateReady` fires when the edge has published a newer bundle than the one this
tab is running, and `App.applyUpdate()` reloads into it.

Implemented by SynQt::ClientUpdate, with SynQt::ClientUpdateAttached as the attached
object behind the handler and SynQt::ClientUpdateAttachedType as the attaching type
registered under the name. It is registered as a QML type rather than bound as a context
property because an attached handler and a context property of the same name cannot
coexist: QML resolves the attaching type first, and the context property would be
shadowed inside JS expressions.

Links into the client only (SynQtClient). Available on both the WebAssembly and the
native desktop build.

@page qmlserver Server

`Server` is the client's view of the connect points its edge owns. `Server.chat` is the
Replica for the connect point named `chat`; the client can only ever see connect points
its own entity declares as consumed, and only those the session's scope allows, so the
object is the authorization boundary as much as it is the accessor.

Implemented by SynQt::ServerAccessor, a `QQmlPropertyMap` so a connect point can be added
by name at runtime as it is acquired. The replicas themselves come from the typed factory
registry (SynQt::acquireReplica) rather than from `acquireDynamic`, because a dynamic
Replica does not propagate property changes in the WebAssembly client.

`Server` is a client-side name. The service-side equivalent, an entity's view of another
entity's connect points, is the owner name capitalized: an entity called `database`
appears to its consumers as `Database`. Those come from SynQt::EntityRuntime.

Links into the client only (SynQtClient).

@page qmlsession Session

`Session` is the signed-in state of the browser, read only from QML apart from the two
verbs: `Session.state`, `Session.identity`, `Session.scope`, `Session.hasScope(name)`,
`Session.login()`, and `Session.logout()`.

Implemented by SynQt::Session. It holds no token. The browser's whole credential is the
session cookie, and every value on this object is state the edge pushed down after it
authorized the connection, which is why nothing here can be forged into an authorization:
the check that matters runs on the owner, against \ref qmlcaller "Caller".

Links into the client only (SynQtClient). The service-side counterpart, the session store
the edge actually keeps, is SynQt::SessionManager.

@page qmlrouter Router

`Router` applies the `routes` list and the `router` fallback from the client config, and
redirects to the fallback when a route names a scope the session lacks.

Implemented by SynQt::Router. Route guards are navigation, not secrecy: they decide which
view is shown, and nothing more. Data reaches the browser only through scope-gated
connect points, so a user who edits their way past a guard finds the view empty rather
than finding data they should not have.

Links into the client only (SynQtClient).

@page qmlcaller Caller

`Caller` is who is calling, present in every connect point slot on the owner side. It
answers the two questions authorization needs and keeps them apart: `Caller.isUser` with
`Caller.session`, `Caller.identity`, `Caller.scope`, and `Caller.hasScope(name)` for a
browser call; `Caller.isEntity` with `Caller.entity`, the name from the verified peer
certificate, for a mesh call.

Implemented by SynQt::Caller. The two identities are never interchangeable. A user-supplied
value is never an entity identity, and on an opt-in local-socket link `Caller.entity` is
trusted by colocation rather than authenticated, which SynQt::Caller reports separately as
`isEntityVerified`.

Links into services only (SynQtService).

@page qmlclient Client

`Client` is the web edge's alias for `Caller.session`. On the edge, where every caller
worth naming arrived from a browser, writing `Client` reads better than writing
`Caller.session`, and it is the same object.

Implemented by SynQt::Caller, the same class behind \ref qmlcaller "Caller"; the alias is
bound as a second context property on the edge only. It does not exist on a service that
is not a web edge, where a caller is another entity and there is no session to alias.

Links into services only (SynQtService).
