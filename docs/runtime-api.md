<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Runtime API reference

The framework puts a small set of objects into your QML. This page is the reference for
each: every member, its type, where it is available, and what it does. The
[programming model](programming-model.md) introduces them.

There is no global `Server`, `Session` or `Client` singleton to import and no base class to
subclass. Each accessor exists only where it makes sense: the client accessors only in the
client entity's QML, `Caller` only inside a connect point slot on the owner, and the
generated Source members only in an owned connect point's implementation.

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

`<Owner>.on<Signal>` attached handlers, which react to a connect point's signals, are
covered in
[handling a connect point's signals](programming-model.md#handling-a-connect-points-signals).
They are generated per contract and available wherever the connect point is consumed.

---

## Client: `Server`

`Server` is the client's handle on its web edge. The edge's connect point members appear
on it by name.

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

- **`Server` always means "the web edge this client talks to",** whatever the edge entity
  is named. It is the client's counterpart to addressing a service by its entity name
  (`Store.find(id)`) elsewhere in the mesh. An entity has one connect point, so the
  accessor has two levels, not three.
- **The accessor exists from the first frame,** before any link is up. A binding on it is
  evaluated at once, holds the member's default until the Replica arrives, then
  re-evaluates. A connect point whose `scope` the session lacks is never acquired, so its
  members keep their defaults and `Server.ready` stays false (see
  [availability and lifecycle](#availability-and-lifecycle) below).
- **A slot with a return type resolves asynchronously,** because the work runs on the
  owner; a slot with no return type is fire and forget. The contract decides this, not
  `Server`.
- **A slot called while `Server.ready` is false does not reach the owner.** A returning
  slot's promise rejects, and a slot with no return type is dropped.

---

## Client: `Session`

`Session` is read-only session state plus the two actions that change it. It never
exposes a secret: the raw session id and any token stay on the edge. QML binds to it to
know whether the visitor is signed in, what they may do, and whether the client is
connected.

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

An edge that accepts the connection and then says nothing also shows as `connecting`.
That is what a hung proxy looks like, and nothing reports it as a failure, so each attempt
has its own deadline; when it passes, the client abandons that attempt and backs off as
usual. So the client never stays in `connecting` forever.

A refused upgrade also shows as `reconnecting`, not as a state of its own. The browser does
not report why a WebSocket handshake failed, so the client cannot tell an edge that is down
from one that rejected its session, and a separate state would be a guess. Authorization
shows where it is visible: an expired or revoked session returns with the default scope,
so `Session.isAuthenticated` goes false and every scope-gated Replica is released.

`Session.identity` fields (the normalized identity, the same object the edge's
mapping hook receives, see [authentication](authentication.md#the-identity-object)):

| Field | Type | Description |
|-------|------|-------------|
| `identity.sub` | string | the stable subject id. Key durable ownership on this, never on email or name. |
| `identity.login` | string | the provider username, when the provider has one. |
| `identity.name` | string | the display name, when the provider has one. |
| `identity.email` | string \| null | the verified email, or `null` when the provider withholds it. Always tolerate null. |

`Session.login(provider?)` starts the login flow on the edge, so the browser never holds
the client secret (see [authentication](authentication.md)). Pass `provider` when more than
one identity provider is configured; otherwise the default (or only) provider is used. In
the browser, it navigates to the edge's `login` route. On a
[native desktop client](desktop.md#signing-in), it opens the system browser at that route
and waits for the answer on a loopback port it holds during the sign-in, while the window
stays as it was.

`Session.logout()` calls the edge's `logout` route, which clears the session on the server
and expires the credential. The session returns to `scopes.default` (anonymous), and every
Replica above that scope is released. While revoking the session, the edge closes the
connections it authorized, and the client reconnects as an anonymous visitor. In the
browser this is a navigation, because the app cannot clear the cookie itself; a native
client holds its own credential and ends the session without leaving the window. In a
project with no `identity`, neither route exists, and calling either says so instead of
requesting a URL the edge does not serve.

The edge sends `Session.scope` and `Session.identity` to the client over the authenticated
`wss` link. The edge holds the session, and the browser holds only an opaque cookie it
cannot read, so the client cannot work either out alone. They arrive as soon as the
connection is accepted, and again whenever the scope changes on a live connection (which
`Caller.setScope` in a slot does). While the link is down, they keep their last value
instead of falling back to anonymous, so a reconnect does not flash a signed-in visitor
through a sign-in screen. A session that has ended comes back anonymous on the next
connection.

`Session.hasScope` is designed to work in a binding:

```qml
Rectangle {
    // Lifts by itself the moment the session is elevated.
    visible: !Session.hasScope("player")
}
```

QML derives a binding's dependencies from the properties it reads, so a binding that only
calls a method has no dependencies and is evaluated once. So `hasScope` is a property whose
value is the check function: reading it registers a dependency on the scope, and you still
call it the same way. On the service side, `Caller.hasScope` is an ordinary method, because
a slot reads it once, for the caller of that call. `Caller`'s own properties do notify
bindings: an elevation changes them, and on a
[shared entity](programming-model.md#how-many-of-an-entity-there-are-shared) so does the
next caller, so a binding on `Caller.scope` or `Caller.identity` re-evaluates instead of
showing the previous caller.

!!! note "Client-side scope checks are UX only"
    Hiding a button with `Session.hasScope(...)` is a convenience, never the security
    boundary. The owner checks every privileged action again, in the slot, against
    [`Caller`](#service-caller). See [security](security.md) for why the check exists in
    both places.

---

## Client: `Router`

`Router` applies the `routes` list and the `router` block from the
[configuration](project-layout-and-config.md#router-and-routes-client-navigation), resolves
the current URL to a page component, and drives the browser's address bar.
[Routes and URLs](routing.md) explains the same subject in prose. It is the same object on
a [native desktop build](desktop.md#navigating-without-an-address-bar), where a history
stack in memory replaces the address bar.

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

`Router` is a context property, not a registered QML type, so these value names are not in
scope in QML: `pageStatus` reads as an integer, counting from zero in the table's order.

### Rendering the current page

An application renders `pageComponent`, with one `Loader` in `Main.qml`:

```qml
Loader {
    anchors.fill: parent
    sourceComponent: Router.pageComponent
}
```

Two paths through one route with a parameter (`/c/spring` then `/c/summer`) resolve to the
same component, and the router keeps the same instance instead of rebuilding it, so the
`Loader` keeps its item and only `path` and `params` change. A view that must react binds
to `Router.params`.

`synqt build` compiles every QML file in the client entity's directory into the client's
QML module, so a route resolves to a real component, as does every helper component and
singleton it uses, with nothing to wire by hand. See
[`routes[].view`](project-layout-and-config.md#router-and-routes-client-navigation) for how
a view is named and where its file goes.

### How a path is matched

A route path is a sequence of segments, each a literal or a capturing `:name` parameter.
When two routes match, the one with more literal segments wins, whatever the declaration
order: `/c/summary` beats `/c/:campaign` even when `/c/:campaign` comes first in
`synqt.yaml`.

Empty segments do not count, so `/c` and `/c/` are one route, and `synqt check`
[rejects declaring both](project-layout-and-config.md#validation). A query string is never
part of a path: it is split off before matching and arrives in `Router.query`.

### Deep links, refreshes, and scope changes

At startup, before the link to the edge opens, the generated client resolves the URL the
page was loaded at. A visitor who bookmarked `/c/summer-sale`, or refreshed on it, lands on
that page, not the home page. The edge helps by
[serving the application shell](security.md#deep-links-and-the-login-resume) for any path
it does not answer itself.

At that moment the session holds only the default scope, because the link to the edge is
not open yet. So a scope-gated deep link resolves `Forbidden` at startup, and resumes as
soon as the real scope arrives.

The router re-resolves the current route on every scope change, in both directions:

- **Gaining a scope** (signing in) opens a route that was refused, then replays a
  remembered destination.
- **Losing a scope** (signing out, or an expired session) moves the visitor off a page they
  may no longer see, and corrects the address bar, so a refresh does not lead back into the
  redirect.

Neither counts as a navigation, so neither adds a history entry.

### Returning to the page that was refused

When a guard refuses a navigation, the router remembers the path (never the query string,
which may carry a token) and replays it once the session can reach it. A visitor who
follows a link to `/admin`, signs in, and then holds `admin` lands on `/admin`, not on the
home page without explanation.

Reading the remembered path clears it, whether or not it was usable, so a stale intent
cannot steer a later visit. Navigating elsewhere does not clear it: a visitor refused at
`/admin` who then browses to `/products` and signs in there still goes to `/admin`, the page
they asked for.

Whoever showed the visitor the link controls the stored path, so it is validated before
use. See [deep links and the login resume](security.md#deep-links-and-the-login-resume) for
the rules and their reasons.

A route guard redirects; it keeps nothing secret. The client is one compiled bundle, so
every view's QML ships to every visitor. Guards steer navigation, and a privileged view's
data still arrives only through scope-gated connect points, which the edge refuses to a
session below their scope. See
[route guards](programming-model.md#route-guards-which-client-views-are-reachable).

---

## Client: `App`

After a deploy, a visitor keeps running the old build as long as their tab stays open.
`App` tells the app when a new build is available.

| Member | Type | Meaning |
|--------|------|---------|
| `App.updateReady` | signal | the edge has a newer client, and it is already cached and ready to apply. |
| `App.applyUpdate()` | action | reload onto the new build. Instant, because the shell cache fetched it before raising the signal. |

If you handle `updateReady`, you choose when to apply it. If you do not, the client reloads
immediately, because an update nobody applies is worse than an interruption: the runtime
reloads when nothing is connected to the signal.

Handle it whenever a reload could lose work. `App.onUpdateReady` is an attached handler, so
it reads like a contract's signal (`Edge.onEaten`) and needs no `Connections` block:

```qml
App.onUpdateReady: updateBanner.visible = true   // "A new version is ready" / Reload
```

then apply it when it is safe:

```qml
Button {
    text: "Reload"
    onClicked: App.applyUpdate()
}
```

This needs `build.client_cache: service_worker` (the default). With `http`, the signal
never fires, and a new build arrives on the next load.

## Client: `Graphics`

Qt Quick draws through the GPU pipeline the browser exposes as WebGL, and some visitors
lack it, because a policy disables it or a driver is blocked. The client still runs, on
Qt's raster adaptation, and `Graphics` tells the app.

| Member | Type | Meaning |
|--------|------|---------|
| `Graphics.isSoftwareRendered` | bool | the client is drawing on the raster adaptation, because this browser offered no accelerated one. |
| `Graphics.hasUnsupportedContent` | bool | something on the current page asked for the accelerated pipeline and could not be drawn. |

You need not handle either. A route marked
[`graphics: accelerated`](project-layout-and-config.md#graphics-which-routes-need-an-accelerated-scene-graph)
is replaced by a notice automatically, and content elsewhere that needs acceleration shows
the same notice over the page, leaving whatever did render in place. Bind to these only to
show your own message:

```qml
Label {
    visible: Graphics.isSoftwareRendered
    text: qsTr("Showing a simplified view")
}
```

Replace the notice itself with `client.graphics_notice` in `synqt.yaml`.

Ordinary 2D Qt Quick renders in software without change.
[Qt Quick 3D](https://doc.qt.io/qt-6/qtquick3d-index.html), `ShaderEffect` and
[Qt Quick Effects](https://doc.qt.io/qt-6/qtquickeffects-qmlmodule.html) draw nothing in
software, which is what the notice explains.

## Client: `Privacy`

What the project's
[`privacy:` block](project-layout-and-config.md#privacy-what-a-visitor-is-told-about-their-data)
declares, and what this visitor answered. It powers `LegalFooter`, `CookieConsent` and
`DataErasureRequest`. An app using those three needs none of this directly; an app writing
its own banner uses it instead.

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

`hasConsent` is a property even though it takes an argument, for the same reason as
[`Session.hasScope`](#client-session): a binding records its dependencies from the
properties it reads, so a plain method call would be evaluated once, while the banner was
still up, and never again.

```qml
Analytics {
    enabled: Privacy.hasConsent("analytics")
}
```

[Privacy and the GDPR](privacy.md) covers the three components and what the defaults are.

## Service: `Caller`

Inside a connect point's slot, `Caller` is whoever made the call. It is one of two things,
and which one is explicit, so an owner authorizes a request without any global state.

| Member | Available when | Type | Description |
|--------|----------------|------|-------------|
| `Caller.isUser` | always | bool | true when the call came from a browser client. Only possible on a web edge connect point. |
| `Caller.isEntity` | always | bool | true when the call came from another entity over a mesh link. |
| `Caller.hasSession` | always | bool | whether there is a person behind this call, the browser's own session when `isUser`, or the session the calling entity is acting for (see [down the chain](#the-session-down-the-chain)). |
| `Caller.session` | `hasSession` | object | the session: `key`, `scope`, `identity`. The same three fields on the edge that authenticated it and down the chain. |
| `Caller.identity` | `hasSession` | object \| null | the caller's normalized identity (same fields as [`Session.identity`](#client-session)), or `null` if anonymous. |
| `Caller.scope` | `hasSession` | string | the one scope name the caller's session holds. |
| `Caller.hasScope(name)` | `hasSession` | bool | whether the caller holds `name`. Hierarchical on the web edge, which is where the vocabulary is, and an exact match on a service (see [scope down the chain](#scope-down-the-chain)). |
| `Caller.setScope(scope)` | `isUser` | action | set the session's scope. Used by the identity flow after login. It rotates the session id on privilege change. The live connection carries on with the new id, and the browser is handed it on its next page load, so a refresh keeps the raised scope rather than starting over. |
| `Caller.emit<Signal>(...)` | `isUser` | action | emit a contract signal back to this one caller (see [targeting](#emitting-a-signal-to-one-caller-versus-all)). |
| `Caller.id` | `isUser` | string | the session key (also `Client.id`), a name derived from the credential, the same as `Caller.session.key`, and never the credential itself. It changes when `setScope` rotates the session. Use it to mark what a session owns. It buys nobody a session. |
| `Caller.entity` | `isEntity` | string | the calling entity's authenticated name, taken from the certificate its mutual-TLS link verified. Authorizing on this alone is correct and complete on every mesh topology except one. See `isEntityVerified`. |
| `Caller.isEntityVerified` | `isEntity` | bool | whether the name was proven by a certificate. True on every mutual-TLS link, which is every link unless the project wrote `transport: local`. False on a local socket link, where the framework supplies the name from the connect point's own consumer list and the operating system confirms only the peer's user. Only a topology that has a local link ever needs to read this. |

Two authorizations at two boundaries, from the
[end to end example](programming-model.md#a-connect-point-implementation-end-to-end): the
edge checks the user, and the database checks the calling entity.

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
    `Caller.entity` is authenticated by certificate on every mesh link, so the single
    check above is complete: the framework decides the name, not the caller. A value a
    user supplies is never an entity identity. The only exception is a link the project
    explicitly moved to `transport: local`, which a slot can refuse with
    `isEntityVerified`. See [security](security.md).

### The session down the chain

Only the first link of a chain authenticates a person. The browser reaches the web edge,
the edge reaches a service, which reaches another. The database above is two links from
the browser, which can never reach it, so the call it answers comes from the edge; on its
own, it would only know that the edge called.

So a connect point that a service consumes carries one thing beyond its contract: the
session the calling entity acts for. The framework fills it in, not the call site, and it
travels along the whole chain, so a service four entities deep still answers a named
person.

```qml
// db/relational/store/Store.qml, reached only by the edge
function insert(row) {
    if (Caller.entity !== "edge") return    // the certificate is the authorization
    // And this is who the edge is answering. `Caller.isUser` is still false, because the
    // caller is the edge. It has somebody behind it.
    Log.info("stored an item", { session: Caller.session.key, sub: Caller.identity.sub })
}
```

What travels is the session's `key`, `scope` and `identity`, never the browser's
credential, which stays on the edge. `key` is derived from the credential: it is the same
string for the same session on every entity, and it cannot be replayed at the edge. A
downstream service keys its own per-session state on it. It changes when the credential
rotates, which happens on a scope change, because an elevated session is a new session.

!!! warning "Authorize the entity, then read the session"
    The certificate authenticated the calling entity. Everything that travels with the call
    is that entity's claim about who it acts for, worth exactly as much as your trust in
    that entity, which the connect point's consumer list already decided. Always authorize
    the entity first, then read the session.

    A browser can never supply this. A point only the client consumes has no such field on
    the wire, and a user's `Caller` ignores one if it arrives, because a session reaches the
    edge as a credential the edge looks up, and nothing inside a call can change it.

#### Scope down the chain

The scope travels with the session; its meaning does not. `scopes.order` is configured
for the edge, the entity that authenticates people and the only one that raises a scope, so
only the edge knows the hierarchy. On a service, `Caller.hasScope("user")` and a `<user>`
member gate are exact matches on the session's one scope name, so `Caller.hasScope("user")`
is false for a moderator. Nothing is silently allowed: an unknown name is refused.

This is by design. Authorizing the person is the edge's job, because the edge knows who
they are. A service authorizes the calling entity by its certificate, and reads the session
to know who the work is for. When a service really must act differently per tier, the edge
decides and says so in the call:

```qml
// web/edge/Edge.qml: the edge holds the vocabulary, so it does the reasoning
function publish(item) {
    if (Caller.hasScope("admin")) {        // hierarchical here: an admin is also a user
        Store.publishImmediately(item)
    } else {
        Store.queueForReview(item)
    }
}
```

Two members instead of one flag, because the two calls are different work, and the service
authorizes each on its own terms. A service that wants its own tiers defines them in its
own configuration; nothing it receives over a link defines a vocabulary.

Two limits apply:

- **Only the edge can call `Caller.setScope`,** so a downstream service cannot elevate a
  session it did not authenticate.
- **A downstream entity's Sources are per calling entity, not per person,** even though it
  answers each call for its person. One mesh link carries one copy of a pushed property or
  model, so state that must differ per browser user belongs on the web edge, which has a
  link per browser. Further down, keep it in the entity's own singleton, under
  `Caller.session.key`.

Outside a call from a consumer (for example a timer on the owner, or the entity's own
singleton) there is no caller, and `Caller` is not in scope. `synqt check` refuses a file
outside a Source that mentions it, because an authorization line that can never run still
looks like one.

### `Client`: the web edge alias

On a web edge's connect points, `Client` is an alias for `Caller` when the caller is a
browser user:

```qml
Client.hasScope("user")     // == Caller.hasScope("user")
Client.identity.email       // == Caller.identity.email
Client.emitRejected(reason) // == Caller.emitRejected(reason)
Client.id                   // the session key
```

`Client` is defined only when `Caller.isUser`. `Caller` is always the general mechanism;
`Client` exists because most edge slots are only ever called by browser users, and `Client`
reads more clearly there.

---

## Owner: the generated Source surface

The owner of a connect point implements it against the Source type the contract generator
emits, named after the owner: an `edge` entity's file has `Edge { ... }` at its root. This
is the only place authoritative state is written. For an `export:` block with
`prop int count`, `model items(string text, string author)`,
`signal rejected(string reason)` and `slot add(string text)`, the owner's Source
exposes:

| Surface | From | Description |
|---------|------|-------------|
| `count = n` | `prop count` | assign to push a new value to every consumer. The owner is the only writer, and consumers get a read-only mirror. |
| `itemsRows: <list>` | `model items(...)` | bind the model to where the rows live, and every change to them republishes. This is the usual form, because the rows almost always live on the entity's singleton, which outlives the Source. |
| `setItems(rows)` | `model items(...)` | the same publish, called rather than bound, for rows that arrive from an event (a reply, a tick). Either way only the declared roles cross. Any extra field on a row (an owner id, a timestamp) is dropped at the boundary and never serializes to a consumer. Each role carries a type, and a row whose value will not convert to it is refused rather than published, naming the model, the role and the row. |
| `rejected(reason)` | `signal rejected` | emit the signal to all consumers of this Source instance. |
| `add(text) { ... }` | `slot add` | the slot body you write. `Caller` is available inside it. |

Both names follow the model name: `model winners(...)` gives `winnersRows` and
`setWinners(rows)`, and `model players(...)` gives `playersRows` and `setPlayers(rows)`.
Today the owner replaces rows wholesale; finer updates would be an optimization behind the
same declaration. Declare a role `var` only where it really holds anything, because the
type is what lets the boundary refuse a wrong value.

These two are the only way into a model. A model flows from the owner to its consumers and
no further, and the boundary refuses a consumer's write, even though the underlying Qt type
has `setData`. To change a row, a consumer calls a slot, where `Caller` exists and the
owner decides. See the
[contract generator](programming-model.md#contracts-the-shape-of-what-may-cross) for how
each `export:` construct is translated.

### Emitting a signal to one caller versus all

There are two ways to emit a contract signal, with different audiences:

- **Calling the Source's signal** (`rejected(reason)`) delivers it to every consumer of that
  Source: on an unshared entity, its one caller; on a shared entity, everybody, because all
  their mirrors follow the one Source.
- **`Caller.emit<Signal>(...)`** (`Caller.emitRejected(reason)`) delivers it to the caller
  currently in the slot.

On an unshared entity both reach the same caller, but prefer `Caller.emit<Signal>`, because
it names its audience.

To reach every consumer, change what they all read: put the state in the entity's own
singleton and let each Source republish it, as described under
[connect points](programming-model.md#connect-points-owned-by-one-entity-consumed-by-others).
That is how one bid reaches every browser watching the auction.

---

## Service: the type helpers

A [typed entity](entities.md) gets one more object, named for what it does: its type's
helper, available in every connect point Source the entity owns. A helper is a thin,
engine-independent front for the [provider](providers.md) the config selected, so the same
Source keeps working when the provider changes. The entity's type, not an import, decides
which helper exists. Entities whose type has none (client, web_edge, service) have none.

| Helper | Injected into | Backed by |
|--------|---------------|-----------|
| `Db` | a `relational` entity | the selected `IPersistenceProvider` (`sqlite`, `postgres`, `mysql`, ...) |
| `Docs` | a `document` entity | the selected `IDocumentProvider` (`memory`, `mongodb`, ...) |
| `Cache` | a `cache` entity | the selected `ICacheProvider` (`memory`, `redis`, ...) |
| `Jobs` | a `jobs` entity | Qt timers and a bounded work queue |
| `Http` | any entity declaring `network.outbound` | `QNetworkAccessManager`, outbound only, restricted to the allowlist |
| `Api` | any entity declaring `network.inbound` | `QHttpServer`, behind the key, origin, size and rate checks |

The entity's
[`network:` block](project-layout-and-config.md#network-what-an-entity-may-reach-and-who-may-reach-it)
grants the last two, not its type: a relational entity that must call an upstream can, and
a gateway that declares nothing cannot. Without a `network:` block, neither name is in
scope.

Errors are reported, never thrown across the QML boundary. A failed call returns an empty
result, and for `Db` also sets `Db.lastError` and emits `Db.errorOccurred`. No helper ever
logs the credentials it was configured with; they stay inside the provider.

### `Db`: relational persistence

| Member | Returns | Description |
|--------|---------|-------------|
| `Db.query(sql, params?)` | list of objects | run a SELECT. One object per row, keyed by column name. Empty on error. |
| `Db.exec(sql, params?)` | object | run an INSERT, UPDATE, DELETE or DDL statement. Returns `{ affected, insertId }`, an empty object on error. |
| `Db.lastError` | string | the message from the most recent failed statement. |
| `Db.errorOccurred(message)` | signal | emitted when a statement fails. |

`params` is an array bound to the `?` placeholders in `sql`, the only way to put a value
into a statement. No overload takes a finished SQL string, so a value can never become
SQL:

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

`filter`, `change` and `options` are plain objects, never an engine query string, so one
Source works with both `memory` and `mongodb`.

### `Cache`: ephemeral key-value

| Member | Returns | Description |
|--------|---------|-------------|
| `Cache.get(key)` | value \| undefined | the stored value, or nothing when the key is missing or expired. A miss is normal rather than an error. |
| `Cache.set(key, value, ttlSeconds?)` | - | store `value`. `ttlSeconds` omitted or `0` means no expiry. |
| `Cache.del(key)` | - | drop the key. |
| `Cache.incr(key, by?)` | int | add `by` (default `1`) atomically and return the new value. The rate-limit counter primitive. |
| `Cache.expire(key, ttlSeconds)` | - | set or replace the TTL on an existing key. `0` or less clears it, exactly as on `set`. It never means "drop the key now". A key whose TTL has already passed is not an existing key, so this drops it rather than reviving it. |

The cache is bounded and evicts. Anything that must survive a restart or an eviction
belongs in a relational entity.

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

When the API needs a key, use a named entry, so no call site holds the key:

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

The runtime reads the value from the entity's environment at startup and attaches it to
the request, so the calling QML never holds the credential and cannot print it.
`synqt check` refuses a header that looks like a credential written as a literal, as it
refuses one in an identity provider's `client_secret`. Headers a request derives from
itself (`Host`, `Content-Length` and the rest of the hop-by-hop set) are refused, because
the transport owns them.

Attach the handler where you make the call, as above. A promise is retired once it has
settled and delivered, so do not store it in a property for later. Keeping one across an
event loop turn and calling `then` later is not supported: the promise belongs to the
entity, which outlives every call, so an unretired promise would be a call that is never
freed.

`Http` is outbound only and verifies TLS. In a release build it refuses a plaintext URL
instead of downgrading, so a gateway cannot silently stop encrypting.

Every call has a deadline: Qt's 30 seconds, measured on transfer rather than the whole
exchange, so a slow reply that keeps arriving is not cut off. A third party that accepts
the connection and then says nothing produces no error by itself, so without the deadline
the error handler for that case would never run and the call would never be freed.

Every call also caps the answer at 16 MiB; past that, the call is rejected and the reply
dropped. The whole body is held in memory before the handler sees it, so without a cap
the responder would decide how much memory a call costs, and an allowlisted third party is
not necessarily a trusted one. The cap is checked while the body arrives, against both the
announced length and the running count, since nothing forces a server to honor its
`Content-Length`.

It refuses any URL outside the prefixes this entity's `network.outbound` lists, and the
error message includes the list, because the mistake is almost always a prefix that does
not cover the path being built. The comparison uses the normalized URL, so no traversal,
percent-encoded or not, can escape a prefix.

The allowlist checks where a call ends up, not only where it starts, so redirects are
checked too. If a third party answers `302` to a place outside the entry the call went
through, the redirect is refused and the call rejected, naming the target. This matters
because a named endpoint's headers are the deployment's credential, and a redirected
request copies the first one, headers included: otherwise an allowlisted host could send
that key anywhere by redirecting. Another entry in the same allowlist counts as elsewhere,
since its key is different. A redirect within the same entry is followed normally.

### `Api`: the inbound HTTP surface

| Member | Returns | Description |
|--------|---------|-------------|
| `Api.get(path, handler)` | - | declare a GET route. `path` is absolute, with `:name` placeholders. |
| `Api.post(path, handler)` | - | declare a POST route. |
| `Api.put(path, handler)` | - | declare a PUT route. |
| `Api.del(path, handler)` | - | declare a DELETE route. |
| `Api.route(method, path, handler)` | - | any other method (PATCH, HEAD). |

Routes are declared once, from the entity's own singleton, and the more literal route
wins whatever the declaration order, so `/lots/open` takes precedence over `/lots/:id`.

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

A handler that returns a value without having answered replies with it as 200, which is
why the synchronous case above is one line. A handler that answers later returns nothing
and calls `reply` or `fail` when ready. Every request is answered exactly once; a second
`reply` is ignored.

`request.client` is the connecting peer, or, if the entity names a proxy in
`network.inbound.trusted_proxies`, the address that proxy forwards. Use it instead of the
forwarding header in `request.headers`, which is whatever the last hop sent, and on a
surface that trusts nobody, whatever the client typed. The built-in rate limit counts the
same address, so a handler that logs or rations by client agrees with the framework.

Access control is settled before the handler runs, so none of it reaches the handler.
`synqt check` refuses an inbound surface without API keys unless it says `public: true`,
and the framework checks the rate limit, the key, the origin and the body size, in that
order, answering the request itself when any check fails. A browser's preflight is
answered between the first two checks, since it carries no key by design: allowed for an
origin in `allowed_origins`, refused for any other, so a page elsewhere never gets to send
the real request.

### `Jobs`: timers and a bounded queue

| Member | Returns | Description |
|--------|---------|-------------|
| `Jobs.every(intervalMs, callback)` | int | run `callback` every `intervalMs`, returning a handle. |
| `Jobs.cancel(handle)` | - | stop the repeating job that `every` returned. |
| `Jobs.enqueue(job)` | bool | queue a one-shot job off the request path. Returns `false` when the queue is full, and the work is dropped rather than buffered without bound. Check it. A job may enqueue another. What it queues runs on a later turn, so the entity keeps answering in between. |
| `Jobs.queued()` | int | how many jobs are pending, for backpressure decisions. A call rather than a property. It is read at the moment a decision is made, and it raises no change signal to bind to. |

Work runs on the entity's own event loop, so a blocking job blocks the entity. A jobs
entity is internal only; a browser can never reach it.

### `Log`: what an entity records about itself

Every service entity has `Log`, whatever its type. The helpers above exist only where their
engine does, but every entity has something to record about what it did.

| Member | Returns | Description |
|--------|---------|-------------|
| `Log.debug(message, attributes?)` | - | the detail worth having while chasing something, off in a normal deployment. |
| `Log.info(message, attributes?)` | - | a fact about what the entity did. |
| `Log.warn(message, attributes?)` | - | something recoverable that someone should see. |
| `Log.error(message, attributes?)` | - | the entity could not do what it was asked. |

`attributes` is a plain map, and values belong there, not in the message. Readers filter
and search the record; `Log.info("saved " + count + " rows")` turns both into a substring
hunt, and `Log.info("saved rows", { rows: count })` does not.

The runtime stamps the entity's name, below anything QML can reach, so no entity can record
under another's name. The same code withholds values: an attribute whose name names a
credential (`password`, `passphrase`, `secret`, `token`, `authorization`, `cookie`,
`credential`, `bearer`, and `api key` or `private key` in any spelling, matched anywhere
in the name, in any case) is recorded as `[redacted]`, so
`Log.warn("refused", { authorization: header })` does not put a bearer token in the
console. It reads names, never values, so it is a backstop, not a license: a credential
under a name that does not say so is recorded like anything else, and the message, which
is prose, is never changed. See [security](security.md#logging-and-observability).

[Monitoring](monitoring.md) covers where records go and who may read them. With no monitor
configured, nothing is recorded, and a call site costs only the level check.

---

## Availability and lifecycle

The framework manages each accessor's lifecycle:

- **A scope-gated connect point is acquired only when the session meets its `scope`.** Below
  that scope the Replica is never handed over, so its slots cannot be called: the gate is
  enforced at acquisition, not by hiding buttons. On a scope change on a live connection
  (`Caller.setScope` in a slot), newly permitted connect points are acquired without a
  reconnect, and those the session no longer meets are withdrawn. On logout, all are
  released. A fronted point (`behind:`) is pointed at the new scope's tier in the same
  step, so the answering entity always matches the current scope.
- **Attached signal handlers (`<Owner>.on<Signal>`) fire only while the connect point is
  live.** Before acquisition, or while `reconnecting`, they do not fire; they resume on
  reconnect.
- **`Router` resolves the page's URL before the link to the edge opens,** and re-resolves
  the current route on every scope change, so a scope-gated page is refused at startup and
  reached once the session holds the scope. See
  [deep links, refreshes, and scope changes](#deep-links-refreshes-and-scope-changes).
- **`Caller` exists only during a slot call from a consumer.** Do not keep it for later;
  read what you need inside the slot.
- **Every link runs a QtRO heartbeat,** so a dropped connection is noticed quickly and
  `Session.state` shows it. See
  [connection lifecycle and offline behavior](programming-model.md#connection-lifecycle-and-offline-behavior).
