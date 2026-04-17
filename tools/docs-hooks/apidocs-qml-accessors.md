@page qmlaccessors QML accessors

<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

@tableofcontents

Most of this runtime exists to put six names into QML. An application never constructs
the C++ classes below. It writes `Server.send(text)` or `Caller.hasScope("admin")`
and the runtime binds the object behind the name. The name and the class are not the
same thing, and searching this reference for `Server` finds a class that is not called
`Server`, so each accessor gets a page of its own here, with every member it puts into QML,
the class that implements each one, and which side of the trust boundary it links into.

The narrative version, written for the QML that calls these rather than for the C++ that
implements them, is the runtime API page on <https://synqt.org/runtime-api/>.

| Accessor | Available in | Implemented by |
|----------|--------------|----------------|
| @subpage qmlapp | client entity QML | SynQt::ClientUpdateAttached |
| @subpage qmlserver | client entity QML | SynQt::ServerAccessor |
| @subpage qmlsession | client entity QML | SynQt::Session |
| @subpage qmlrouter | client entity QML | SynQt::Router |
| @subpage qmlcaller | any owner slot, any entity | SynQt::Caller |
| @subpage qmlclient | web edge owner slots | SynQt::Caller |

@page qmlapp App

`App` is the running client itself, as opposed to \qmlServer, which is everything the
client reaches over the wire. The client is conveyed to every visitor, so a deploy can
leave someone running an old build for as long as their tab stays open. `App` is how the
application finds out and decides when to reload.

@section qmlapp_members Members

| Member | Type | Implemented by | Description |
|--------|------|----------------|-------------|
| `App.onUpdateReady` | attached signal handler | SynQt::ClientUpdateAttached::updateReady | The edge has published a newer client than the one this tab is running, and it is already cached and ready to apply. |
| `App.applyUpdate()` | action | SynQt::ClientUpdateAttached::applyUpdate | Reload onto the new build. It is immediate, because the shell cache fetched the new bundle before the signal was raised. |

@section qmlapp_notes Notes

Handling `updateReady` takes ownership of the timing. Handling nothing reloads the client
the moment an update lands, on the grounds that an update nobody applies is worse than an
interruption. SynQt::ClientUpdate::notifyUpdateReady implements that by counting the
receivers connected to the signal and reloading through SynQt::ClientUpdate::reloadPage
when there are none.

It needs `build.client_cache: service_worker`, which is the default. Under `http` the
signal never fires, because there is no cache to have fetched the new build ahead of
time. A new build arrives on the next load instead.

@section qmlapp_implementation Behind the name

SynQt::ClientUpdate holds the state and does the reloading, SynQt::ClientUpdateAttached is
the attached object each `App.onUpdateReady` handler binds to, and
SynQt::ClientUpdateAttachedType is the attaching type registered under the QML name.
SynQt::registerClientUpdate performs that registration.

It is a QML type rather than a context property because an attached handler and a context
property of the same name cannot coexist. QML resolves the attaching type first, and the
context property is then shadowed inside every JavaScript expression that names it.

Links into the client only (SynQtClient). Available on both the WebAssembly and the native
desktop build.

@page qmlserver Server

`Server` is the client's view of the one connect point its edge owns, so its members are
that contract's members. `Server.send(text)` calls the `send` slot the edge exported. The
client can only ever see connect points its own entity declares as consumed, and only
those the session's scope allows, so the object is an authorization boundary as much as it
is an accessor.

@section qmlserver_members Members

| Member | Type | Description |
|--------|------|-------------|
| `Server.<member>` | per the contract | Each `prop`, `model`, `signal` and `slot` the owner's `export:` block declares. Properties and models are read-only mirrors of the owner's Source. Slots are callable, and every call is a request the owner may refuse. |
| `Server.ready` | bool | SynQt::ConsumerBase::ready, true once the bound Replica has completed its QtRO handshake. |

There is no fixed member list beyond that, because of what the class is.
SynQt::ServerAccessor is a `QQmlPropertyMap` holding one consumer facade per owner this
client reaches, keyed by that owner's name, and `Server` is the entry belonging to the
edge.

The facade is built at construction rather than when a link first comes up, because a
binding written against `Server` is evaluated on the first frame and a name that resolves
to nothing until the handshake finishes is a binding that never runs again. A connect
point whose `scope` the session does not hold is never acquired at all, so its members
hold their defaults and `Server.ready` stays false. Both resume on reconnect.

@section qmlserver_implementation Behind the name

SynQt::ServerAccessor, bound to the node by SynQt::ServerAccessor::bindNode and populated
from SynQt::ServerAccessor::onReplicaInitialized as each Replica reports ready.

The replicas come from the typed factory registry (SynQt::acquireReplica) rather than from
`QRemoteObjectNode::acquireDynamic`, because a dynamic Replica does not propagate property
changes in the WebAssembly client.

`Server` is a client-side name, and a well-known alias. Whatever the edge entity is
called, the client addresses it as `Server`. The service-side equivalent, one
entity's view of another's connect point, is the owner's entity name capitalized, so an
entity called `database` appears to its consumers as `Database`. SynQt::EntityRuntime
sets those up rather than this class.

Links into the client only (SynQtClient).

@page qmlsession Session

`Session` is the signed-in state of the browser, read only from QML apart from the two
verbs that change it. It holds no token. The browser's whole credential is the session
cookie, and every value here is state the edge pushed down after it authorized the
connection, which is why nothing on this object can be forged into an authorization. The
check that matters runs on the owner, against \qmlCaller.

@section qmlsession_members Members

| Member | Type | Implemented by | Description |
|--------|------|----------------|-------------|
| `Session.state` | string | SynQt::Session::state | The connection and authorization state, one of the values below. |
| `Session.scope` | string | SynQt::Session::scope | The one scope name the session holds. Under hierarchical scopes a name higher in the order satisfies a lower one, and under set-based scopes a check succeeds only on the name itself. Prefer `hasScope` to comparing it. |
| `Session.identity` | object \| null | SynQt::Session::identity | The normalized identity when authenticated, `null` when anonymous. |
| `Session.isAuthenticated` | bool | SynQt::Session::isAuthenticated | Convenience for `Session.identity !== null`. |
| `Session.hasScope(name)` | bool | SynQt::Session::hasScope | Whether the session holds `name`. Under hierarchical scopes a higher scope satisfies a lower one. Safe to bind, because a binding that calls it re-evaluates when the scope moves. |
| `Session.login(provider)` | action | SynQt::Session::login | Start the edge login flow. `provider` is optional; pass it when more than one identity provider is configured. |
| `Session.logout()` | action | SynQt::Session::logout | End the session. The scope returns to the anonymous default and any Replica above it is released. |

`Session.state` is one of `offline` (the starting state), `connecting`, `connected`, or
`reconnecting`. All four are transport states, including the case where the edge refused
the upgrade. The browser does not report why a WebSocket handshake failed, so a rejected
session credential is not distinguishable from an edge that is down. An expired or
revoked session shows up instead as `Session.isAuthenticated` going false when the client
reconnects anonymously, and as its scope-gated replicas being released.

@section qmlsession_implementation Behind the name

SynQt::Session. The setters (SynQt::Session::setState, SynQt::Session::setScope,
SynQt::Session::setIdentity) are C++ only, called by the client runtime as the edge
reports state. QML sees the properties as read-only.

`hasScope` is a property whose value is the check rather than an invokable method, and
that is what lets a scope-gated binding work. QML records what a binding depends on from
the properties it reads and from nothing else, so a binding that only called a method
would be evaluated once and never again. The call spelling is unchanged. `login()` and `logout()` do not act
directly either. They raise SynQt::Session::loginRequested and
SynQt::Session::logoutRequested for SynQt::SynClient to carry out.

Links into the client only (SynQtClient). The service-side counterpart, the session store
the edge keeps, is SynQt::SessionManager.

@section qmlsession_warning Client-side scope checks are for the interface only

Hiding a control with `Session.hasScope(...)` is a convenience, never the boundary. Every
privileged action is checked again on the owner, inside the slot, against \qmlCaller.

@page qmlrouter Router

`Router` applies the `routes` list and the `router` block from the client config,
resolves the current URL to a page component, and drives the address bar. It redirects
to the configured fallback when a route names a scope the session lacks.

@section qmlrouter_members Members

| Member | Type | Implemented by | Description |
|--------|------|----------------|-------------|
| `Router.path` | string | SynQt::Router::path | The current application path, without the query string and without the router base. Read-only from QML. After a redirect it is the fallback path rather than the one that was asked for. |
| `Router.params` | object | SynQt::Router::params | The path parameters the matched route captured, percent-decoded. Empty for a route without parameters. On a redirect the refused route's captures are dropped and the fallback route's own take their place, which is nothing at all for the usual parameterless fallback. |
| `Router.query` | object | SynQt::Router::query | The decoded query string of the current URL. Cleared whenever the navigation ends somewhere other than the route that was asked for, a guard refusal and an unmatched path alike, so a query addressed to that page never reaches the fallback. |
| `Router.pageComponent` | Component \| null | SynQt::Router::pageComponent | The component for the current route's view, ready to hand to a `Loader`. Null when there is no view to show. |
| `Router.pageStatus` | enumeration | SynQt::Router::pageStatus | Why the current page is the one showing, one of SynQt::Router::Ready, SynQt::Router::Loading, SynQt::Router::Forbidden, SynQt::Router::NotFound, SynQt::Router::Error, or SynQt::Router::Unsupported. A remote page reports `Loading` while the edge is being asked for it. |
| `Router.pageSeed` | object | SynQt::Router::pageSeed | The seed the edge sent for the current page, a read-only map. For a remote page it is whatever the route's seed hook returned, so a delivered page can paint its first frame before its connect points arrive. It is empty for a compiled-in view and kept across a `notModified` refetch. |
| `Router.go(path)` | action | SynQt::Router::go | Navigate to `path` and push a history entry. When the matched route declares a scope the session lacks, the router goes to the configured fallback instead and reports `Forbidden`. |
| `Router.replace(path)` | action | SynQt::Router::replace | Navigate without adding a history entry. The current entry is rewritten, so `back()` skips the page being left. |
| `Router.back()` | action | SynQt::Router::back | Go back one history entry, as the browser's Back button does. |
| `Router.forward()` | action | SynQt::Router::forward | Go forward one history entry. |
| `Router.resumeAfterLogin()` | action | SynQt::Router::resumeAfterLogin | Go to the page the visitor was refused before signing in, if the session can now reach it, and forget it either way. The runtime already calls it on every scope change; an application calls it only when it establishes a session by a route of its own. |

`Router` is bound as a context property rather than registered as a QML type, so the
`PageStatus` names are not in scope in QML. `pageStatus` reads there as its integer value,
counting from zero in the order declared on SynQt::Router::PageStatus.

@section qmlrouter_implementation Behind the name

SynQt::Router, which holds the route table as a list of SynQt::RouteConfig compiled into
SynQt::RoutePattern, notifies QML through SynQt::Router::pathChanged and
SynQt::Router::pageChanged, and owns a SynQt::BrowserHistory.

SynQt::RoutePattern decides both matching and precedence. Precedence is
SynQt::RoutePattern::literalSegmentCount rather than declaration order, so `/c/summary` wins over
`/c/:campaign` however the table was written. SynQt::Router::applyRoutes sorts on it once
rather than searching for it per navigation.

SynQt::BrowserHistory is the only class that knows a browser has a history. On WebAssembly
it drives the History API through Emscripten, and on a desktop build it keeps an equivalent
stack in memory, so SynQt::Router::back needs no platform branch.

SynQt::Router::start resolves the path the application was opened at, which the generated
client main calls once the root object exists and before the link to the edge opens. The
session holds only its default scope at that moment, so a scope-gated deep link resolves
`Forbidden` there and is replayed by SynQt::Router::resumeAfterLogin when the real scope
arrives. The refused path is remembered and validated through SynQt::ResumePath, whose
SynQt::ResumePath::isAcceptable is what keeps an attacker-supplied link from turning the
resume into an open redirect.

SynQt::Router::resolveRemote is a virtual returning false here, the hook for a page this
class cannot build itself. A remote page, one the web edge delivers at run time rather than
compiling into the bundle, is resolved through it. SynQt::RemotePageLoader fetches the page
over the client's link, caches it by content hash, and hands SynQt::Router the component,
while SynQt::QmlPalette enforces `router.palette`, the set of QML modules a delivered page
may import. Both sit behind `Router`; an application sees only the resolved
`pageComponent` and the `pageSeed` the edge sent with it.

A route guard only redirects. The client is one compiled
bundle, so every view's QML ships to every visitor. Guards decide which view is shown and
nothing more. The data behind a privileged view arrives only through scope-gated connect
points, so a user who edits their way past a guard finds the view empty rather than
finding data they should not have.

Links into the client only (SynQtClient).

@page qmlcaller Caller

`Caller` is whoever is calling, present in every connect point slot on the owner side. It
answers the two questions authorization needs and keeps them apart, a browser user or
another entity. Which of the two it is, is always explicit, and the two are never
interchangeable.

@section qmlcaller_members Members

| Member | Type | Implemented by | Description |
|--------|------|----------------|-------------|
| `Caller.isUser` | bool | SynQt::Caller::isUser | True when the call came from a browser client, which is only possible on a web edge connect point. |
| `Caller.isEntity` | bool | SynQt::Caller::isEntity | True when the call came from another entity over a mesh link. |
| `Caller.isEntityVerified` | bool | SynQt::Caller::isEntityVerified | Entity callers. True when the name came from a certificate the mutual-TLS handshake verified, false when the link is an opt-in local socket and the name is trusted by colocation instead. |
| `Caller.entity` | string | SynQt::Caller::entity | Entity callers. The calling entity's name, taken from the certificate its link verified. |
| `Caller.hasSession` | bool | SynQt::Caller::hasSession | Whether a person stands behind this call: the browser's own session for a user, or the session a calling entity acts for. |
| `Caller.id` | string | SynQt::Caller::id | User callers. The session key, a name derived from the credential and never the credential itself. |
| `Caller.session` | object | SynQt::Caller::session | When `hasSession`. The session record: `key`, `scope`, `identity`. |
| `Caller.identity` | object \| null | SynQt::Caller::identity | When `hasSession`. The normalized identity (`sub`, `login`, `name`, `email`), or `null` when anonymous. |
| `Caller.scope` | string | SynQt::Caller::scope | When `hasSession`. The granted scope. Prefer `hasScope` to comparing it. |
| `Caller.hasScope(name)` | bool | SynQt::Caller::hasScope | When `hasSession`. Whether the caller holds `name`, hierarchically on the edge and as an exact match on a service. |
| `Caller.setScope(scope)` | action | SynQt::Caller::setScope | User callers. Set the session's scope. Used by the identity flow after login, and it rotates the session id on a privilege change. |
| `Caller.emit<Signal>(...)` | action | SynQt::Caller::emitSignal | User callers. Emit one of the contract's signals back to this one caller rather than to every consumer. The generated name is sugar over `emitSignal`. |

Which half of the table applies is never ambiguous. `isUser` and `isEntity` are the two
mutually exclusive cases, and reading a member of the other half is a mistake the owner
should not be making rather than a value it should be interpreting.

Outside a call that originated from a consumer (an owner-side timer mutating shared state,
for instance) `Caller` is out of scope. The owner writes its Source and QtRO fans the
change out.

@section qmlcaller_implementation Behind the name

SynQt::Caller, built per call by SynQt::Caller::forUser or SynQt::Caller::forEntity and
bound as a context property on the slot. A contract can supply its own subclass through
SynQt::Caller::registerCallerFactory, which is what gives `emit<Signal>` its per-contract
signal names. SynQt::Caller::setScopeOrder carries the project's scope configuration in.

`Caller.entity` is certificate-authenticated on every mesh link by default. On an opt-in
local-socket link it is trusted by colocation instead, which is a weaker claim and is why
SynQt::Caller reports it separately as `isEntityVerified` rather than quietly presenting
the two as the same thing. A user-supplied value is never an entity identity.

Links into services only (SynQtService).

@page qmlclient Client

`Client` is the web edge's alias for \qmlCaller when the caller is a browser user. On the
edge, where most slots are only ever called from a browser, `Client.hasScope("user")`
reads better than `Caller.hasScope("user")`, and it is the same object.

@section qmlclient_members Members

Every member of \qmlCaller that is available when `isUser` is true, under the name
`Client`. `Client.id` is the session key, `Client.identity.email` the caller's verified
address, `Client.emit<Signal>(...)` a signal to that one caller.

@section qmlclient_implementation Behind the name

SynQt::Caller, the same class and the same instance behind \qmlCaller. The alias is bound
as a second context property, on the edge only. It does not exist on a service that is not
a web edge, where a caller is another entity and there is no session to alias.

Links into services only (SynQtService).
