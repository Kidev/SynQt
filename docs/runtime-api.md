<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Runtime API reference

The framework injects a small set of objects into your QML. This page is the
exact reference for each one: every member, its type, where it is available, and
what it does. The [programming model](programming-model.md) is the narrative
introduction, and this page is the reference.

There is no global `Server`, `Session`, or `Client` singleton to import and no
base class to subclass. Each accessor is scoped to where it makes sense. The
client-side accessors exist only in the client entity's QML, `Caller` exists only
inside a connect point slot on the owner, and the generated Source surface exists only
in an owned connect point's implementation.

## Which accessor exists where

| Accessor | Available in | Purpose |
|----------|--------------|---------|
| `Server` | client entity QML | the connect points this client consumes, by name |
| `Session` | client entity QML | read-only session state, plus `login()` / `logout()` |
| `Router` | client entity QML | scope-gated navigation over the route table, and the browser's address bar |
| `App` | client entity QML | the running client itself: whether a newer build is ready, and applying it |
| `Privacy` | client entity QML | what the project declared about a visitor's data, and what this visitor answered |
| `Caller` | any owner slot (any entity) | who invoked this slot: a browser user, or a calling entity |
| `Client` | web edge owner slots | alias for `Caller` when the caller is a browser user |
| generated Source | an owned connect point's implementation | the owner-side write surface (`set<Model>`, property setters, signals) |
| `Db`, `Docs`, `Cache`, `Jobs` | a typed entity's QML | the helper that type provides, one per entity (see [the type helpers](#service-the-type-helpers)) |
| `Http`, `Api` | an entity with a `network:` block | outbound calls within its allowlist, and the inbound surface it serves |
| `Log` | every service entity's QML | what this entity records about what it did |

`<Owner>.on<Signal>` attached handlers (for reacting to a connect point's
signals) are covered in [Handling a connect point's signals](programming-model.md#handling-a-connect-points-signals);
they are generated per contract and available wherever that connect point is
consumed.

---

## Client: `Server`

`Server` is the client's handle on the web edge it is attached to. Each connect
point the client consumes appears on it by the connect point's configured name.

```qml
Label   { text: "Items: " + Server.count }   // a live property
ListView { model: Server.items }             // a live model
Button  { onClicked: Server.add(input.text) } // a slot call (a request)
```

| Member | Type | Description |
|--------|------|-------------|
| `Server.<member>` | per the contract | each `prop`, `model`, `signal` and `slot` the edge's `export:` block declares. Properties and models are read-only mirrors of the owner's Source. Slots are callable and are always requests the owner may refuse. |
| `Server.ready` | bool | the framework's own. True once the edge is hosting this connect point for this browser. It goes false on a disconnect and true again on the reconnect. |

Notes:

- `Server` is the well-known alias for "the web edge this client talks to,"
  whatever that edge entity is named. It is the client-side counterpart
  of addressing a service by its entity name (`Store.find(id)`) elsewhere in the
  mesh. An entity has one connect point, so the accessor is two levels and not
  three.
- The accessor exists from the first frame, before any link is up. A binding
  written against it is evaluated immediately and holds the member's default until
  the Replica arrives, then re-evaluates. A connect point with a `scope` the
  session does not hold is never acquired at all, so its members stay at their
  defaults for an under-scoped user and `Server.ready` stays false (see
  [Availability and lifecycle](#availability-and-lifecycle) below).
- A slot with a return type resolves asynchronously (the work happens on the
  owner). A slot with no return type is fire-and-forget. This is a property of the
  contract rather than of `Server`.

---

## Client: `Session`

`Session` is read-only session state plus the two actions that change it. It never
exposes a secret. The raw session id and any token live at the edge rather than in the
client. Session state is what QML binds to for whether the visitor is signed in, what
they may do, and whether the client is connected.

| Member | Type | Description |
|--------|------|-------------|
| `Session.state` | string | the connection/authorization state. One of the values in the table below. |
| `Session.scope` | string | the one scope name the session holds. With hierarchical scopes (the default) a name higher in `order` satisfies a lower one. With set-based scopes a check succeeds only on the name itself. Prefer `hasScope` for checks. |
| `Session.hasScope(name)` | bool | whether the session holds `name`. With hierarchical scopes a higher scope satisfies a lower one (`hasScope("user")` is true for a moderator). Safe to bind, because a binding that calls it re-evaluates when the scope moves. |
| `Session.identity` | object \| null | the normalized identity when authenticated, `null` when anonymous. Fields below. |
| `Session.isAuthenticated` | bool | convenience for `Session.identity !== null`. |
| `Session.login(provider?)` | action | start the edge login flow. See below. |
| `Session.logout()` | action | end the session. See below. |

`Session.state` values:

| Value | Meaning |
|-------|---------|
| `offline` | the starting state, before the client has tried to reach the edge. |
| `connecting` | the wss connection to the edge is being established. |
| `connected` | the link is up and replicas are live. |
| `reconnecting` | the link dropped or was refused, and the client is retrying with capped exponential backoff. Replicas report not-ready, and bindings hold their last values. |

An edge that accepts the connection and then says nothing is `connecting` too. That is
what a hung proxy looks like, and nothing reports it as a failure, so each attempt
carries a deadline of its own. When it passes the client gives up on that attempt and
backs off like any other. `connecting` is therefore always a state the client leaves.

A refused upgrade is `reconnecting` too, rather than a state of its own. The browser does
not report why a WebSocket handshake failed, so a client cannot tell an edge that
is down from an edge that rejected its session, and a state that claimed to know
would be guessing. Authorization is observed where it is visible. An
expired or revoked session comes back with the default scope, so
`Session.isAuthenticated` goes false and every scope-gated Replica is released.

`Session.identity` fields (the normalized identity, the same object the edge's
mapping hook receives, see [authentication](authentication.md#the-identity-object)):

| Field | Type | Description |
|-------|------|-------------|
| `identity.sub` | string | the stable subject id. Key durable ownership on this, never on email or name. |
| `identity.login` | string | the provider username, when the provider has one. |
| `identity.name` | string | the display name, when the provider has one. |
| `identity.email` | string \| null | the verified email, or `null` when the provider withholds it. Always tolerate null. |

`Session.login(provider?)` starts the login flow at the edge, so the browser
never holds the client secret (see [pitfall: OAuth cannot run in the
browser](authentication.md)). `provider` is optional. Pass it when more than one
identity provider is configured, otherwise the default (or only) provider is used.
In the browser this navigates to the edge's `login` route. On a
[native desktop client](desktop.md#signing-in) it opens the system browser at that
route and waits for the answer on a loopback port it holds for the length of the
sign-in. The window stays where it was.

`Session.logout()` calls the edge's `logout` route, which clears the session
server-side and expires the credential. The session returns to `scopes.default`
(anonymous), and any Replica above the new scope is released. The edge closes the
connections that session authorized as it revokes it, and the client reconnects as an
anonymous visitor. In the browser this is a navigation, because the cookie is not the
app's to clear. A native client holds its own credential and ends the session without
leaving the window. A project that configures no `identity` has no route for either
action, and calling one says so rather than requesting a URL the edge does not serve.

The edge tells the client both `Session.scope` and `Session.identity`,
over the same authenticated `wss` link everything else rides. The edge holds the
whole session and the browser holds an opaque cookie it cannot read, so nothing in
the client could work either of them out on its own. They arrive as soon as the
connection is accepted, and again whenever the scope changes under a live
connection, which is what `Caller.setScope` in a slot does. While the link is down
they hold their last value rather than falling back to anonymous, so a reconnect
does not flash a signed-in visitor through a sign-in screen. A session that has
ended comes back anonymous on the next connection.

`Session.hasScope` is meant to be used in a binding, and is built so that it can be:

```qml
Rectangle {
    // Lifts by itself the moment the session is elevated.
    visible: !Session.hasScope("player")
}
```

QML works out what a binding depends on from the properties it reads, so a binding
that only calls a method has no dependencies and is evaluated once and never
again. `hasScope` is therefore a property whose value is the check rather than a plain
method. Reading it registers the dependency on the scope, and the call
spelling is unchanged. `Caller.hasScope` on the service side is an ordinary method,
because a slot reads it once, in the slot, against the caller of that call.
`Caller`'s own properties do change under a binding. An elevation moves them, and on a
[shared entity](programming-model.md#how-many-of-an-entity-there-are-shared) so does
the next caller, so a binding that reads `Caller.scope` or `Caller.identity` is told and
re-evaluated rather than left showing the caller before this one.

!!! note "Client-side scope checks are UX only"
    Hiding a button with `Session.hasScope(...)` is a convenience, never the
    security boundary. Every privileged action is checked again on the owner,
    inside the slot, against [`Caller`](#service-caller). See
    [security](security.md) for why the check exists in both places.

---

## Client: `Router`

`Router` applies the `routes` list and the `router` block from config
([configuration](project-layout-and-config.md#router-and-routes-client-navigation)),
resolves the current URL to a page component, and drives the browser's address bar.
[Routes and URLs](routing.md) is the same subject written as prose rather than as the
table below.
It is the same object on a [native desktop build](desktop.md#navigating-without-an-address-bar),
where an in-memory stack stands in for the address bar.

| Member | Type | Description |
|--------|------|-------------|
| `Router.path` | string | the current application path, without the query string and without `router.base`. Read-only. It changes as a result of navigation, and after a guard redirect it is the fallback path rather than the one that was asked for. |
| `Router.params` | object | the path parameters the matched route captured, percent-decoded (`/c/:campaign` navigated to `/c/summer%20sale` gives `{ campaign: "summer sale" }`). Empty for a route with no parameters. On a redirect the refused route's captures are dropped and the fallback route's own captures take their place, which is nothing at all for the usual parameterless fallback. |
| `Router.query` | object | the decoded query string of the current URL (`?page=2&q=hat` gives `{ page: "2", q: "hat" }`). Cleared whenever the navigation ends somewhere other than the route that was asked for, whether a guard refused it or nothing matched, so a query addressed to that page never reaches the fallback. |
| `Router.pageComponent` | Component \| null | the component for the current route's view, ready to hand to a `Loader`. `null` when the route has no view to show. |
| `Router.pageStatus` | enumeration | why the current page is the one showing: `Ready`, `Loading`, `Forbidden`, `NotFound`, `Error`, or `Unsupported`. Values below. |
| `Router.pageSeed` | object | the seed the edge sent for the current page, a read-only map. For a [remote page](remote-pages.md) it is whatever the route's [seed hook](remote-pages.md#the-page-seed-painting-the-first-frame) returned, so a delivered page can paint real content on its first frame before its connect points arrive. It is empty for a compiled-in view and for a remote page whose route declares no `seed`. It is kept across a `notModified` refetch, so a new parameterization of one page paints the new seed rather than the old page's data. |
| `Router.go(path)` | action | navigate to `path` and add a history entry. If the matched route declares a `scope` the session lacks, the router goes to `router.fallback` instead and reports `Forbidden`. |
| `Router.replace(path)` | action | navigate without adding a history entry. The current entry is rewritten, so `back()` skips the page being left. |
| `Router.back()` | action | go back one history entry, exactly as the browser's Back button does. |
| `Router.forward()` | action | go forward one history entry. |
| `Router.resumeAfterLogin()` | action | go to the page the visitor was refused before signing in, if the session can now reach it, and forget it either way. The framework already calls this on every scope change; call it yourself only if your app establishes a session by some route of its own. |

`Router.path` and `Router.params` change together, and `Router.query` changes with
them, so one binding on any of the three sees a consistent set.

`Router.pageStatus` values:

| Value | Meaning |
|-------|---------|
| `Ready` | the matched route's view is built and showing. |
| `Loading` | the view is still being built. A view compiled into the bundle is built synchronously, so a route pointing at one never reports this. A [remote page](remote-pages.md) does, while the edge is being asked for it and the reply has not arrived. |
| `Forbidden` | a route matched, but it declares a `scope` the session lacks. `path` is now `router.fallback` and the fallback's view is showing. The refused path is remembered for [after login](#returning-to-the-page-that-was-refused). |
| `NotFound` | nothing in the route table matched. `path` is now `router.fallback`, the fallback's view is showing, and the query the unmatched path carried is dropped. |
| `Unsupported` | the route declares [`graphics: accelerated`](project-layout-and-config.md#graphics-which-routes-need-an-accelerated-scene-graph) and this browser gave Qt no accelerated scene graph, so the page cannot be drawn. Unlike a scope refusal this is not a redirect. `path` is still the path that was asked for, and `pageComponent` is the notice, so a `Loader` bound to it shows the notice where the page would have been. |
| `Error` | there is no page to show. A compiled-in view failed to load, because it does not compile or because its URL names nothing, or a [remote page](remote-pages.md) arrived but could not be shown, because no loader is present to resolve it, or the delivered page was refused by the [palette](remote-pages.md#the-palette-what-a-delivered-page-may-import) or would not compile. An edge refusal is not this. A scope refusal reports `Forbidden` and a route the edge does not know reports `NotFound`. A route that declares neither a `view` nor a `remote` in `synqt.yaml` never becomes a page at all. `synqt check` reports it, and `synqt build` refuses to generate it. `Error` also wins over `Forbidden` and `NotFound` when it is the fallback's own view that failed, because a broken fallback is the more urgent fact and is what an app has to surface first. |

`Router` is bound as a context property rather than as a registered QML type, so
the value names above are not in scope in QML. `pageStatus` reads there as its
integer value, counting from zero in the order of the table.

### Rendering the current page

`pageComponent` is the member an application renders. One `Loader` in
`Main.qml` is the whole of it:

```qml
Loader {
    anchors.fill: parent
    sourceComponent: Router.pageComponent
}
```

Two paths through one parameterized route (`/c/spring` then `/c/summer`) resolve to
the same component, and the router hands back the same instance rather than
rebuilding it, so the `Loader` keeps its item alive and only `path` and `params`
change. A view that wants to react to that binds `Router.params`.

`synqt build` compiles every QML file under the client entity's directory into the
client's QML module, so a route resolves to a real component, and so does every
helper component and singleton that component reaches, with nothing to wire up by
hand. See
[`routes[].view`](project-layout-and-config.md#router-and-routes-client-navigation)
for how a view is named and where its file goes.

### How a path is matched

A route path is a sequence of segments, each either a literal or a `:name`
parameter that captures. When two routes both match, the one with more literal
segments wins, whatever order they are declared in. `/c/summary` beats
`/c/:campaign` even when `/c/:campaign` comes first in `synqt.yaml`.

An empty segment is not a segment, so `/c` and `/c/` are one route and `synqt check`
[rejects declaring both](project-layout-and-config.md#validation). A query string is
never part of a path. It is split off before matching and arrives in
`Router.query`.

### Deep links, refreshes, and scope changes

The generated client resolves the URL the page was loaded at, at boot, before the
link to the edge opens. A visitor who bookmarked `/c/summer-sale`, or who pressed
refresh on it, lands on that page rather than on the home page. The edge cooperates
by [serving the application shell](security.md#deep-links-and-the-login-resume) for
any path it does not answer itself.

At that moment the session holds only the default scope, because the link to the
edge has not opened yet. A scope-gated deep link therefore resolves `Forbidden`
at boot, and is resumed the moment the real scope arrives.

The router re-resolves the current route on every scope change, in both directions:

- Gaining scope (a sign-in) promotes a route that was refused, and then replays
  a remembered destination.
- Losing scope (a sign-out, or an expired session) evicts the visitor from a
  page they may no longer see, instead of leaving them sitting on it. The address
  bar is corrected with it, so a refresh does not walk straight back into the
  redirect.

Neither is a navigation, so neither adds a history entry.

### Returning to the page that was refused

When a guard refuses a navigation, the router remembers the path (never the query
string, which may carry a token) and replays it once the session can reach it. A
visitor who follows a link to `/admin`, signs in, and holds `admin` afterwards ends
up on `/admin` rather than on the home page with no explanation.

The remembered path is cleared by being read, whether or not it turned out to be
usable, so a stale intent cannot steer a later visit. It is not cleared by
navigating somewhere else. A visitor bounced off `/admin` who then browses to
`/products` and signs in there is still taken to `/admin`. The page they were refused
is the one they asked for, and whatever they looked at while signed out was them
waiting to be let in.

The stored path is under the control of whoever put the link in front of the
visitor, so it is validated before anything acts on it. The rules, and why they are
what they are, are in
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

A route guard redirects and keeps nothing secret. The client is one
compiled bundle, so every view's QML ships to every visitor. Guards steer
navigation, while the data behind a privileged view still arrives only through
scope-gated connect points the edge refuses to an under-scoped session. This is
covered in [route guards](programming-model.md#route-guards-which-client-views-are-reachable).

---

## Client: `App`

The client is conveyed to every visitor, so a deploy can leave someone running an old
build for as long as their tab stays open. `App` is how the app finds out.

| Member | Type | Meaning |
|--------|------|---------|
| `App.updateReady` | signal | the edge has a newer client, and it is already cached and ready to apply. |
| `App.applyUpdate()` | action | reload onto the new build. Instant, because the shell cache fetched it before raising the signal. |

If you handle `updateReady`, you own the timing. If you handle nothing, the client
reloads immediately, on the grounds that an update nobody applies is worse than an
interruption. The runtime implements that by checking whether anything is connected to
the signal and reloading when nothing is.

Handle it whenever a reload could lose work. `App.onUpdateReady` is an attached
handler, so it reads like a contract's own signal (`Arena.onEaten`) and needs no
`Connections` block:

```qml
App.onUpdateReady: updateBanner.visible = true   // "A new version is ready" / Reload
```

and apply it once the moment is safe:

```qml
Button {
    text: "Reload"
    onClicked: App.applyUpdate()
}
```

Requires `build.client_cache: service_worker` (the default). Under `http` the signal
never fires, and a new build arrives on the next load instead.

## Client: `Graphics`

Qt Quick draws through the GPU pipeline the browser exposes as WebGL, and some visitors
have no such pipeline, because it can be disabled by policy or blocked for a driver. The client
runs anyway, on Qt's raster adaptation, and `Graphics` is how the app finds out.

| Member | Type | Meaning |
|--------|------|---------|
| `Graphics.isSoftwareRendered` | bool | the client is drawing on the raster adaptation, because this browser offered no accelerated one. |
| `Graphics.hasUnsupportedContent` | bool | something on the current page asked for the accelerated pipeline and could not be drawn. |

Neither has to be handled. A route marked
[`graphics: accelerated`](project-layout-and-config.md#graphics-which-routes-need-an-accelerated-scene-graph)
is replaced by a notice on its own, and content that turns out to need acceleration
anywhere else raises the same notice over the page, leaving everything that did render in
place. Bind to these only to say something of your own:

```qml
Label {
    visible: Graphics.isSoftwareRendered
    text: qsTr("Showing a simplified view")
}
```

Replace the notice itself with `client.graphics_notice` in `synqt.yaml`.

Ordinary 2D Qt Quick renders in software without any change.
[Qt Quick 3D](https://doc.qt.io/qt-6/qtquick3d-index.html), `ShaderEffect` and
[Qt Quick Effects](https://doc.qt.io/qt-6/qtquickeffects-qmlmodule.html) do not. They draw
nothing at all, which is what the notice explains.

## Client: `Privacy`

What the project's [`privacy:` block](project-layout-and-config.md#privacy-what-a-visitor-is-told-about-their-data)
declared, and what this visitor said back. It is the accessor behind `LegalFooter`,
`CookieConsent` and `DataErasureRequest`. An app that uses those three needs none of this
directly, and an app writing its own banner uses it instead of them.

| Member | Type | Meaning |
|--------|------|---------|
| `Privacy.policyUrl` | string | where the privacy policy is, empty when the project declared none. |
| `Privacy.legalNoticeUrl` | string | where the legal notice is, on the same terms. |
| `Privacy.contact` | string | the controller contact. |
| `Privacy.retentionDays` | int | how long the project keeps personal data, so a page can state the period without a second copy of the number. |
| `Privacy.categories` | list | the non-essential cookie categories the project declared. Empty in a project that declares none. |
| `Privacy.consentRequired` | bool | whether there is anything to ask about. False while `categories` is empty, because the session credential is exempt. |
| `Privacy.consentAnswered` | bool | whether this visitor has answered. |
| `Privacy.granted` | list | what they allowed, a subset of `categories`. |
| `Privacy.hasConsent(name)` | bool | whether this category is permitted. False until they say otherwise. |
| `Privacy.erasureOffered` | bool | whether the project offers an erasure request. |
| `Privacy.accept(list)` | call | record an answer. Categories the project never declared are dropped. |
| `Privacy.acceptAll()` | call | record every declared category as allowed. |
| `Privacy.acceptNecessaryOnly()` | call | record an answer allowing none of them. |
| `Privacy.withdrawConsent()` | call | forget the answer, so the banner asks again. |

`hasConsent` is read as a property even though it takes an argument, for the reason
[`Session.hasScope`](#client-session) is. A binding records its dependencies from the
properties it reads, so a plain method call would be evaluated once, while the banner was
still up, and never again.

```qml
Analytics {
    enabled: Privacy.hasConsent("analytics")
}
```

[Privacy and the GDPR](privacy.md) covers the three components and what the defaults are.

## Service: `Caller`

Inside a connect point's slot, `Caller` is whoever invoked it. It is one of two
things, and which one is explicit, so an owner authorizes a request without any
ambient global.

| Member | Available when | Type | Description |
|--------|----------------|------|-------------|
| `Caller.isUser` | always | bool | true when the call came from a browser client. Only possible on a web edge connect point. |
| `Caller.isEntity` | always | bool | true when the call came from another entity over a mesh link. |
| `Caller.hasSession` | always | bool | whether there is a person behind this call: the browser's own session when `isUser`, or the session the calling entity is acting for (see [down the chain](#the-session-down-the-chain)). |
| `Caller.session` | `hasSession` | object | the session: `key`, `scope`, `identity`, and on the edge that authenticated it, `id`. |
| `Caller.identity` | `hasSession` | object \| null | the caller's normalized identity (same fields as [`Session.identity`](#client-session)), or `null` if anonymous. |
| `Caller.scope` | `hasSession` | string | the one scope name the caller's session holds. |
| `Caller.hasScope(name)` | `hasSession` | bool | whether the caller holds `name` (hierarchical where configured). |
| `Caller.setScope(scope)` | `isUser` | action | set the session's scope. Used by the identity flow after login; rotates the session id on privilege change. The live connection carries on with the new id, and the browser is handed it on its next page load, so a refresh keeps the raised scope rather than starting over. |
| `Caller.emit<Signal>(...)` | `isUser` | action | emit a contract signal back to **this one caller** (see [targeting](#emitting-a-signal-to-one-caller-versus-all)). |
| `Caller.id` | `isUser` | string | the session id (also `Client.id`). |
| `Caller.entity` | `isEntity` | string | the calling entity's authenticated name, taken from the certificate its mutual-TLS link verified. Authorizing on this alone is correct and complete on every mesh topology except one: see `isEntityVerified`. |
| `Caller.isEntityVerified` | `isEntity` | bool | whether the name was proven by a certificate. True on every mutual-TLS link, which is every link unless the project wrote `transport: local`. False on a local socket link, where the framework supplies the name from the connect point's own consumer list and the operating system confirms only the peer's *user*. Only a topology that has a local link ever needs to read this. |

Two authorizations at two boundaries, from the [end-to-end
example](programming-model.md#a-connect-point-implementation-end-to-end): the edge
checks the user, the database checks the calling entity.

```qml
// web/edge/Edge.qml: the edge authorizes a user
function add(text) {
    if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in first."); return }
    Store.insert({ text: text.trim(), ownerSub: Caller.identity.sub })
}

// db/relational/store/Store.qml: the database authorizes the calling entity
function insert(row) {
    if (Caller.entity !== "edge") return    // only the edge may write
    Db.exec("INSERT INTO items(text, owner_sub) VALUES(?,?)", [row.text, row.ownerSub])
}
```

!!! warning "Two identity systems, never conflated"
    `Caller.isUser` (a browser session, identified by login and scope) and
    `Caller.isEntity` (a service, identified by certificate) are separate systems.
    `Caller.entity` is certificate-authenticated on every mesh link, which is what
    makes the one check above complete. The framework decides the name, and the
    caller does not. A user-supplied value is never an entity identity. The single
    exception is a link the project explicitly moved to `transport: local`, and
    `isEntityVerified` is how a slot refuses that. See [security](security.md).

### The session down the chain

Only the first link of a chain authenticates a person. The browser reaches
the web edge, the edge reaches a service, that service reaches another. The database in
the example above is two links from the browser and can never be reached by it, so the
call it answers is the edge's. Without help, all it would know is that the edge called.

So a connect point that a service consumes carries one thing more than its contract
declares, the session the calling entity is acting for. The framework fills it in,
rather than the call site, and it travels for as long as the chain does, so a service four
entities deep still answers a named person.

```qml
// db/relational/store/Store.qml, reached only by the edge
function insert(row) {
    if (Caller.entity !== "edge") return    // the certificate is the authorization
    // And this is who the edge is answering. `Caller.isUser` is still false, because the
    // caller is the edge. It has somebody behind it.
    Log.info("stored an item", { session: Caller.session.key, sub: Caller.identity.sub })
}
```

What travels is the session's `key`, its `scope` and its `identity`. Never the browser's
credential, which stays at the edge. `key` is derived from it, is the same string for the
same session on every entity that sees it, and cannot be replayed at the edge. It is what
a downstream service keys its own per-session state on. It changes when the credential
rotates, which happens on a scope change, because an elevated session is a different
session.

!!! warning "Authorize the entity, then read the session"
    The certificate authenticated the calling entity. Everything that rides along with the
    call is that entity's word about who it is acting for, and is worth exactly as much as
    trusting that entity, which is a decision the connect point's consumer list already
    made. Authorize the entity first, always, and read the session after.

    A browser can never do this. A point only the client consumes has no such field on the
    wire at all, and a user's `Caller` ignores one if it somehow arrives, because a session reaches
    the edge as a credential the edge looks up, and nothing inside a call can change who
    that is.

Two limits apply. `Caller.setScope` is the edge's alone: a downstream service
cannot elevate a session it did not authenticate. And a downstream entity answers each
call for whoever it is for, but its Sources are still one per calling entity, not one per
person: one mesh link can carry one copy of a pushed property or model, so state that must
differ per browser user belongs on the web edge, which does have a link per browser. Below
that, keep it in the entity's own singleton under `Caller.session.key`.

Outside a call that originated from a consumer (for example an owner-side timer, or the
entity's own singleton) there is no caller, and `Caller` is not in scope at all. `synqt
check` refuses a file that names it outside a Source, because an authorization line that
cannot run still reads like one.

### `Client`: the web edge alias

On web edge connect points, `Client` is a convenience alias for `Caller` when the
caller is a browser user, so edge code reads directly:

```qml
Client.hasScope("user")     // == Caller.hasScope("user")
Client.identity.email       // == Caller.identity.email
Client.emitRejected(reason) // == Caller.emitRejected(reason)
Client.id                   // the session id
```

`Client` is only defined when `Caller.isUser`. The general mechanism is always
`Caller`. `Client` exists because most edge slots are only ever called by a
browser user and reading `Client` there is clearer than reading `Caller`.

---

## Owner: the generated Source surface

The owner of a connect point implements it against the Source type the contract
generator emits, named `<Owner>Source`. This is the only place authoritative
state is written. For an `export:` block reading `prop int count`, `model items(string
text, string author)`, `signal rejected(string reason)` and `slot add(string text)`, the
owner's Source exposes:

| Surface | From | Description |
|---------|------|-------------|
| `count = n` | `prop count` | assign to push a new value to every consumer. The owner is the only writer, and consumers get a read-only mirror. |
| `itemsRows: <list>` | `model items(...)` | bind the model to where the rows live, and every change to them republishes. This is the usual form, because the rows almost always live on the entity's singleton, which outlives the Source. |
| `setItems(rows)` | `model items(...)` | the same publish, called rather than bound, for rows that arrive from an event (a reply, a tick). Either way only the declared roles cross. Any extra field on a row (an owner id, a timestamp) is dropped at the boundary and never serializes to a consumer. Each role carries a type, and a row whose value will not convert to it is refused rather than published, naming the model, the role and the row. |
| `rejected(reason)` | `signal rejected` | emit the signal to all consumers of this Source instance. |
| `add(text) { ... }` | `slot add` | the slot body you write. `Caller` is available inside it. |

Both names follow the model name. `model winners(...)` gives `winnersRows` and
`setWinners(rows)`, and `model players(...)` gives `playersRows` and `setPlayers(rows)`.
Replacing the rows wholesale is the owner surface today. Finer-grained updates are an
optimization behind the same declaration. Declare a role `var` where it carries
anything, and only there, because a type is what makes the refusal above possible.

Those two are the only way into a model. It travels from the owner to its consumers and
no further, and a consumer's write is refused at the boundary even though the Qt type
underneath has a `setData`. A consumer that wants a row changed calls a slot, which is
where `Caller` exists and where the owner decides. See the [contract
generator](programming-model.md#contracts-the-shape-of-what-may-cross) for how
each `export:` construct lowers.

### Emitting a signal to one caller versus all

There are two ways to emit a contract signal, and the difference is the audience:

- Calling the Source's signal (`rejected(reason)`) delivers it to every consumer of that
  Source. On an entity that is not shared, that is the one caller it belongs to. On a
  shared entity it is everybody, because their mirrors all follow the one Source.
- `Caller.emit<Signal>(...)` (`Caller.emitRejected(reason)`) delivers it to the one caller
  currently in the slot.

On an entity that is not shared the two reach the same caller, and `Caller.emit<Signal>`
is the habit to keep, because it names the audience it means.

To reach every consumer, change what they are all reading. Put the state in the entity's
own singleton and let each Source republish from it. That is the arrangement described
under [connect points](programming-model.md#connect-points-owned-by-one-entity-consumed-by-others), and it is how one bid reaches
every browser watching the auction.

---

## Service: the type helpers

A [typed entity](entities.md) gets one more injected object, named for what it
does: the helper its type provides, available in every connect point Source that
entity owns. A helper is a thin, engine-agnostic front for the
[provider](providers.md) the config selected, which is why the same Source keeps
working when the provider changes. The entity's type decides which helper exists,
rather than an import. An entity whose type has none (client, web_edge, service) has none of them.

| Helper | Injected into | Backed by |
|--------|---------------|-----------|
| `Db` | a `relational` entity | the selected `IPersistenceProvider` (`sqlite`, `postgres`, `mysql`, ...) |
| `Docs` | a `document` entity | the selected `IDocumentProvider` (`memory`, `mongodb`, ...) |
| `Cache` | a `cache` entity | the selected `ICacheProvider` (`memory`, `redis`, ...) |
| `Jobs` | a `jobs` entity | Qt timers and a bounded work queue |
| `Http` | any entity declaring `network.outbound` | `QNetworkAccessManager`, outbound only, restricted to the allowlist |
| `Api` | any entity declaring `network.inbound` | `QHttpServer`, behind the key, origin, size and rate checks |

The last two are granted by the entity's
[`network:` block](project-layout-and-config.md#network-what-an-entity-may-reach-and-who-may-reach-it)
rather than by its type, so a relational entity that has to call one upstream can, and
a gateway that declares nothing cannot. An entity with no `network:` block has neither
name in scope.

Errors are reported, never thrown across the QML boundary. A failed call returns an
empty result and, for `Db`, sets `Db.lastError` and emits `Db.errorOccurred`. No
helper ever logs the credentials it was configured with. Those stay inside the
provider.

### `Db`: relational persistence

| Member | Returns | Description |
|--------|---------|-------------|
| `Db.query(sql, params?)` | list of objects | run a SELECT. One object per row, keyed by column name. Empty on error. |
| `Db.exec(sql, params?)` | object | run an INSERT, UPDATE, DELETE or DDL statement. Returns `{ affected, insertId }`, an empty object on error. |
| `Db.lastError` | string | the message from the most recent failed statement. |
| `Db.errorOccurred(message)` | signal | emitted when a statement fails. |

`params` is an array bound to the `?` placeholders in `sql`, and it is the only way
to get a value into a statement. There is no overload that takes a finished SQL
string, so a value can never become SQL:

```qml
// Correct: the value is a parameter.
Db.query("SELECT id, text FROM items WHERE owner_sub = ? LIMIT ?", [sub, 20])

// There is no API for this. Concatenation is how injection happens.
Db.query("SELECT id, text FROM items WHERE owner_sub = '" + sub + "'")
```

### `Docs`: schemaless documents

| Member | Returns | Description |
|--------|---------|-------------|
| `Docs.insert(collection, document)` | id \| null | insert one document, returning its new id. `null` on failure. |
| `Docs.find(collection, filter?, options?)` | list of objects | every document matching `filter`, in storage order. Empty when nothing matches and when the call fails, so treat empty as "nothing to show" rather than as "it worked". |
| `Docs.update(collection, filter, change)` | int | apply `change` to every document matching `filter`, returning how many changed. |
| `Docs.remove(collection, filter)` | int | remove every document matching `filter`, returning how many went. |

`filter`, `change` and `options` are plain objects, never an engine query string.
That is what keeps one Source working across `memory` and `mongodb`.

### `Cache`: ephemeral key-value

| Member | Returns | Description |
|--------|---------|-------------|
| `Cache.get(key)` | value \| undefined | the stored value, or nothing when the key is missing or expired. A miss is normal rather than an error. |
| `Cache.set(key, value, ttlSeconds?)` | - | store `value`. `ttlSeconds` omitted or `0` means no expiry. |
| `Cache.del(key)` | - | drop the key. |
| `Cache.incr(key, by?)` | int | add `by` (default `1`) atomically and return the new value. The rate-limit counter primitive. |
| `Cache.expire(key, ttlSeconds)` | - | set or replace the TTL on an existing key. `0` or less clears it, exactly as on `set`. It never means "drop the key now". A key whose TTL has already passed is not an existing key, so this drops it rather than reviving it. |

The cache is bounded and evicts. Anything that has to survive a restart or an
eviction belongs in a relational entity rather than here.

### `Http`: outbound calls, within the allowlist

| Member | Returns | Description |
|--------|---------|-------------|
| `Http.api(name)` | endpoint | the named `network.outbound` entry, its base URL, and the headers the runtime attaches to every call under it. |
| `Http.get(url, headers?)` | promise | issue a GET. |
| `Http.post(url, body?, headers?)` | promise | issue a POST. A body that is not a string is sent as JSON. |
| `Http.put(url, body?, headers?)` | promise | issue a PUT. |
| `Http.del(url, headers?)` | promise | issue a DELETE. |
| `endpoint.get(path?, headers?)` | promise | the same four, with `path` resolved against the endpoint's base. |
| `endpoint.url` | string | the base this endpoint resolves against. |
| `promise.then(onOk, onError?)` | - | `onOk({ status, body, json })` on success, `onError(message)` on failure. `json` is there when the reply said it was JSON. Settles once, and a handler attached in the same statement fires as soon as it settles. |

```qml
Http.get("https://api.example.com/rates")
    .then(response => { rates.value = response.json.usd },
          message => { rates.error = message })
```

A named entry is the form to reach for when the API wants a key, because the key is
then not something a call site holds:

```yaml
    network:
      outbound:
        - name: rates
          url: https://api.example.com/
          headers:
            x-api-key: env:RATES_API_KEY
```

```qml
Http.api("rates").get("v1/rates").then(response => { rates.value = response.json.usd })
```

The value is read from the entity's environment when the entity starts and attached to
the request by the runtime, so the QML that makes the call never holds the credential
and cannot print it. `synqt check` refuses a credential-looking header written as a
literal, for the same reason it refuses one in an identity provider's `client_secret`.
The headers a request derives from itself (`Host`, `Content-Length`, and the rest of the
hop-by-hop set) are refused, because the transport owns those.

Attach the handler where the call is made, as above. A promise is retired once it has
settled and delivered, so it is not an object to store in a property and come back to
later. Keeping one across an event loop turn and calling `then` on it then is the one
use this does not support, because the promise belongs to the entity, which
outlives every call, so a promise nobody retires is a call nobody can ever free.

`Http` is outbound only and verifies TLS. In a release build it refuses a plaintext
URL rather than downgrading, so a gateway cannot quietly stop encrypting.

Every call has a deadline (Qt's own 30 seconds, on transfer rather than on the whole
exchange, so a slow reply that keeps arriving is not cut off). A third party that
accepts the connection and then answers nothing is not an error and never becomes one,
so without it the error handler written for exactly that case would never run and the
call would never be freed.

Every call also has a ceiling of 16 MiB of answer, after which the call is rejected and the
reply abandoned. The whole body is held in memory before a handler is given it, so without
one the memory a call costs is decided by whoever is answering, and an allowlisted third
party is not the same thing as a trusted one. It is checked while the body is arriving and
against the announced length as well as the running count, since a `Content-Length` is not a
promise anybody has to keep.

It also refuses any URL that is not under one of the prefixes this entity's
`network.outbound` names, and the rejection message carries the list, because the
mistake is nearly always a prefix that does not cover the path being composed. The
comparison is against the normalized URL, so a traversal or a percent-encoded one
cannot spell its way out of a prefix.

The allowlist is a check on where a call ends up, and not only on where it starts, so a
redirect is put through it too. A third party that answers `302` to somewhere outside
the entry the call was made through has its redirect refused and the call rejected,
naming the place it tried to reach. This matters because the headers on a named endpoint
are the deployment's credential, and a redirected request is a copy of the first one,
headers included. Without the check, an allowlisted host could send that key anywhere
by redirecting, and the entity would follow. Another entry in the same allowlist
counts as elsewhere here, since its key is not this one's. A redirect that stays under
the same entry is followed as normal.

### `Api`: the inbound HTTP surface

| Member | Returns | Description |
|--------|---------|-------------|
| `Api.get(path, handler)` | - | declare a GET route. `path` is absolute, with `:name` placeholders. |
| `Api.post(path, handler)` | - | declare a POST route. |
| `Api.put(path, handler)` | - | declare a PUT route. |
| `Api.del(path, handler)` | - | declare a DELETE route. |
| `Api.route(method, path, handler)` | - | any other method (PATCH, HEAD). |

Routes are declared once, from the entity's own singleton, and the more literal route
wins whichever was declared first, so `/lots/open` takes precedence over `/lots/:id`.

The handler is called with one argument, the request:

| Member | Type | Description |
|--------|------|-------------|
| `request.method` | string | GET, POST, PUT, DELETE, ... |
| `request.path` | string | the routed path, without the query string. |
| `request.params` | object | the `:name` placeholders this route captured. |
| `request.query` | object | the decoded query string pairs. |
| `request.headers` | object | request headers, lower-cased. The API key header is removed before a handler sees it. |
| `request.body` | object \| string | the parsed JSON for an `application/json` request, the raw text otherwise. |
| `request.client` | string | who is calling, as an address. |
| `request.reply(body, status?)` | - | answer. A map or a list is sent as JSON; anything else as text. Default status 200. |
| `request.fail(status, message)` | - | answer with `{"error": message}` and that status. |

```qml
Api.get("/lots/:id", request => {
    Books.lot(request.params.id)
        .then(lot => request.reply(lot),
              error => request.fail(404, error));
});

Api.get("/health", () => { return { ok: true }; });
```

A handler that returns a value and has not answered yet replies with it as 200, which
is what makes the synchronous case the one-liner above. A handler that will answer
later returns nothing and calls `reply` or `fail` when it can. Every request is
answered exactly once. A second `reply` is ignored rather than writing twice.

`request.client` is the peer that connected, unless the entity named a proxy in
`network.inbound.trusted_proxies`, in which case it is the address that proxy said is
behind it. Use it rather than the forwarding header in `request.headers`. That one is
whatever the last hop sent, and on a surface that trusts nobody it is whatever the
client typed. It is also the address the built-in rate limit counts, so a handler that
logs or rations by client agrees with the framework rather than keeping a second
opinion.

Nothing about who may call reaches the handler, because it was settled before the
handler existed. `synqt check` refuses an inbound surface with no API keys unless it
says `public: true`, and the framework checks the rate limit, the key, the origin and
the body size in that order, answering the request itself when any of them fails.

### `Jobs`: timers and a bounded queue

| Member | Returns | Description |
|--------|---------|-------------|
| `Jobs.every(intervalMs, callback)` | int | run `callback` every `intervalMs`, returning a handle. |
| `Jobs.cancel(handle)` | - | stop the repeating job that `every` returned. |
| `Jobs.enqueue(job)` | bool | queue a one-shot job off the request path. Returns `false` when the queue is full, and the work is dropped rather than buffered without bound. Check it. A job may enqueue another. What it queues runs on a later turn, so the entity keeps answering in between. |
| `Jobs.queued()` | int | how many jobs are pending, for backpressure decisions. A call rather than a property. It is read at the moment a decision is made, and it raises no change signal to bind to. |

Work runs on the entity's own event loop, so a job that blocks blocks that entity.
A jobs entity is internal only, and nothing on it is ever reachable from a browser.

### `Log`: what an entity records about itself

Every service entity has this one, whatever its type. The helpers above are in scope only
where the engine behind their type is. An entity always has something to say about what it
did, so this one is everywhere.

| Member | Returns | Description |
|--------|---------|-------------|
| `Log.debug(message, attributes?)` | - | the detail worth having while chasing something, off in a normal deployment. |
| `Log.info(message, attributes?)` | - | a fact about what the entity did. |
| `Log.warn(message, attributes?)` | - | something recoverable that someone should see. |
| `Log.error(message, attributes?)` | - | the entity could not do what it was asked. |

`attributes` is a plain map, and the values belong in it rather than in the message:
whoever reads the record filters and searches it, and `Log.info("saved " + count + " rows")`
makes both a substring hunt where `Log.info("saved rows", { rows: count })` does not.

Which entity said it is stamped by the runtime, past anything QML can reach, so an entity
cannot record itself under another entity's name. The same place holds a value back: an
attribute whose name names a credential (`password`, `secret`, `token`, `authorization`,
`cookie`, `credential`, `api_key`, `private_key`, `bearer`, matched anywhere in the name
and in any case) is recorded as `[redacted]`, so `Log.warn("refused", { authorization:
header })` does not put a bearer token in the console. It reads names and never values, so
it is a backstop rather than a licence: a credential under a name that does not say so is
recorded like anything else, and the message is prose and is never touched. See
[security](security.md#logging-and-observability).

Where the records go and who may read them is [monitoring](monitoring.md); with no monitor
configured nothing is recorded and the level check is all a call site costs.

---

## Availability and lifecycle

The framework owns each accessor's lifecycle:

- A scope-gated connect point is acquired only when the session meets its
  `scope`. Below that scope the Replica is never handed over, so
  its slots cannot be called at all; the gate is enforced at acquisition, not by
  hiding buttons. On a scope upgrade (`Caller.setScope` after login) the newly
  permitted connect points are acquired; on logout they are released.
- Attached signal handlers (`<Owner>.on<Signal>`) fire only while the connect
  point is live. Before acquisition, or during `reconnecting`, they simply do not
  fire, and they resume on reconnect.
- `Router` resolves the URL the page was loaded at before the link to the edge
  opens, and re-resolves the current route on every scope change, so a scope-gated
  page is refused at boot and reached again once the session actually holds the
  scope. See [deep links, refreshes, and scope
  changes](#deep-links-refreshes-and-scope-changes).
- `Caller` exists only for the duration of a slot invocation that originated from a
  consumer. Do not capture it and use it later; read what you need from it inside
  the slot.
- Every link uses a QtRO heartbeat, so a dropped connection is noticed promptly and
  `Session.state` reflects it. See [connection lifecycle and offline
  behavior](programming-model.md#connection-lifecycle-and-offline-behavior).
