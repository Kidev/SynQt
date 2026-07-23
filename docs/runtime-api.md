# Runtime API reference

The framework injects a small set of objects into your QML. This page is the
exact reference for each one: every member, its type, where it is available, and
what it does. The [programming model](programming-model.md) is the narrative
introduction; this is the lookup table you keep open while writing code.

There is no global `Server`, `Session`, or `Client` singleton to import and no
base class to subclass. Each accessor is scoped to where it makes sense: the
client-side accessors exist only in the client entity's QML; `Caller` exists only
inside a connect point slot on the owner; the generated Source surface exists only
in an owned connect point's implementation.

## Which accessor exists where

| Accessor | Available in | Purpose |
|----------|--------------|---------|
| `Server` | client entity QML | the connect points this client consumes, by name |
| `Session` | client entity QML | read-only session state, plus `login()` / `logout()` |
| `Router` | client entity QML | scope-gated navigation over the route table, and the browser's address bar |
| `App` | client entity QML | the running client itself: whether a newer build is ready, and applying it |
| `Caller` | any owner slot (any entity) | who invoked this slot: a browser user, or a calling entity |
| `Client` | web edge owner slots | alias for `Caller` when the caller is a browser user |
| generated Source | an owned connect point's implementation | the owner-side write surface (`set<Model>`, property setters, signals) |

`<Contract>.on<Signal>` attached handlers (for reacting to a connect point's
signals) are covered in [Handling a connect point's signals](programming-model.md#handling-a-connect-points-signals);
they are generated per contract and available wherever that connect point is
consumed.

---

## Client: `Server`

`Server` is the client's handle on the web edge it is attached to. Each connect
point the client consumes appears on it by the connect point's configured name.

```qml
Label   { text: "Items: " + Server.todo.count }   // a live property
ListView { model: Server.todo.items }             // a live model
Button  { onClicked: Server.todo.add(input.text) } // a slot call (a request)
```

| Member | Type | Description |
|--------|------|-------------|
| `Server.<name>` | Replica | the live Replica of the connect point named `<name>` in `synqt.yaml`. Properties and models are read-only mirrors of the owner's Source; slots are callable and are always requests the owner may refuse. |

Notes:

- `Server` is the well-known alias for "the web edge this client talks to,"
  whatever that edge entity is actually named. It is the client-side counterpart
  of addressing a service by its entity name (`Database.items`) elsewhere in the
  mesh.
- A connect point appears on `Server` only once its Replica has been **acquired**.
  A connect point with a `scope` the session does not hold is never acquired, so
  `Server.<name>` is not live for an under-scoped user (see
  [Availability and lifecycle](#availability-and-lifecycle) below). Bindings to it
  simply hold their default until it becomes ready, and resume on reconnect.
- A slot with a return type resolves asynchronously (the work happens on the
  owner); a slot with no return type is fire-and-forget. This is a property of the
  contract, not of `Server`.

---

## Client: `Session`

`Session` is read-only session state plus the two actions that change it. It never
exposes a secret: the raw session id and any token live at the edge, not in the
client. Session state is what QML binds to for "am I signed in," "what may I do,"
and "are we connected."

| Member | Type | Description |
|--------|------|-------------|
| `Session.state` | string | the connection/authorization state. One of the values in the table below. |
| `Session.scope` | string \| list | the session's granted scope. A single name with hierarchical scopes (the default); the set of granted names with set-based scopes. Prefer `hasScope` for checks. |
| `Session.hasScope(name)` | bool | whether the session holds `name`. With hierarchical scopes a higher scope satisfies a lower one (`hasScope("user")` is true for a moderator). |
| `Session.identity` | object \| null | the normalized identity when authenticated, `null` when anonymous. Fields below. |
| `Session.isAuthenticated` | bool | convenience for `Session.identity !== null`. |
| `Session.login(provider?)` | action | start the edge login flow. See below. |
| `Session.logout()` | action | end the session. See below. |

`Session.state` values:

| Value | Meaning |
|-------|---------|
| `connecting` | the first wss connection to the edge is being established. |
| `connected` | the link is up and replicas are live. |
| `reconnecting` | the link dropped; the client is retrying with capped exponential backoff. Replicas report not-ready; bindings hold their last values. |
| `denied` | the edge rejected the session credential (expired or revoked). The client routes back through login rather than retrying. |
| `offline` | no usable connection and not currently retrying. |

`Session.identity` fields (the normalized identity; the same object the edge's
mapping hook receives, see [authentication](authentication.md#the-identity-object)):

| Field | Type | Description |
|-------|------|-------------|
| `identity.sub` | string | the stable subject id. Key durable ownership on this, never on email or name. |
| `identity.login` | string | the provider username, when the provider has one. |
| `identity.name` | string | the display name, when the provider has one. |
| `identity.email` | string \| null | the verified email, or `null` when the provider withholds it. Always tolerate null. |

`Session.login(provider?)` starts the login flow **at the edge**, so the browser
never holds the client secret (see [pitfall: OAuth cannot run in the
browser](authentication.md)). `provider` is optional; pass it when more than one
identity provider is configured, otherwise the default (or only) provider is used.
In the browser this navigates to the edge's `login` route; on a
[native desktop client](desktop.md#signing-in) it opens the system browser and
receives the session back over a loopback redirect.

`Session.logout()` calls the edge's `logout` route, which clears the session
server-side and expires the credential. The session returns to `scopes.default`
(anonymous), and any Replica above the new scope is released.

!!! note "Client-side scope checks are UX only"
    Hiding a button with `Session.hasScope(...)` is a convenience, never the
    security boundary. Every privileged action is checked again on the owner,
    inside the slot, against [`Caller`](#service-caller). This duplication is
    intentional; see [security](security.md).

---

## Client: `Router`

`Router` applies the `routes` list and the `router` block from config
([configuration](project-layout-and-config.md#router-and-routes-client-navigation)),
resolves the current URL to a page component, and drives the browser's address bar.
It is the same object on a [native desktop build](desktop.md#navigating-without-an-address-bar),
where an in-memory stack stands in for the address bar.

| Member | Type | Description |
|--------|------|-------------|
| `Router.path` | string | the current application path, without the query string and without `router.base`. Read-only; it changes as a result of navigation, and after a guard redirect it is the fallback path, not the one that was asked for. |
| `Router.params` | object | the path parameters the matched route captured, percent-decoded (`/c/:campaign` navigated to `/c/summer%20sale` gives `{ campaign: "summer sale" }`). Empty for a route with no parameters. On a redirect the refused route's captures are dropped and the fallback route's own captures take their place, which is nothing at all for the usual parameterless fallback. |
| `Router.query` | object | the decoded query string of the current URL (`?page=2&q=hat` gives `{ page: "2", q: "hat" }`). Cleared whenever the navigation ends somewhere other than the route that was asked for, whether a guard refused it or nothing matched, so a query addressed to that page never reaches the fallback. |
| `Router.pageComponent` | Component \| null | the component for the current route's view, ready to hand to a `Loader`. `null` when the route has no view to show. |
| `Router.pageStatus` | enumeration | why the current page is the one showing: `Ready`, `Loading`, `Forbidden`, `NotFound`, or `Error`. Values below. |
| `Router.pageSeed` | object | the seed the edge sent for the current page, a read-only map. For a [remote page](remote-pages.md) it is whatever the route's [seed hook](remote-pages.md#the-page-seed-painting-the-first-frame) returned, so a delivered page can paint real content on its first frame before its connect points arrive; empty for a compiled-in view and for a remote page whose route declares no `seed`. It is kept across a `notModified` refetch, so a new parameterization of one page paints the new seed rather than the old page's data. |
| `Router.go(path)` | action | navigate to `path` and add a history entry. If the matched route declares a `scope` the session lacks, the router goes to `router.fallback` instead and reports `Forbidden`. |
| `Router.replace(path)` | action | navigate without adding a history entry: the current entry is rewritten, so `back()` skips the page being left. |
| `Router.back()` | action | go back one history entry, exactly as the browser's Back button does. |
| `Router.forward()` | action | go forward one history entry. |
| `Router.resumeAfterLogin()` | action | go to the page the visitor was refused before signing in, if the session can now reach it, and forget it either way. The framework already calls this on every scope change; call it yourself only if your app establishes a session by some route of its own. |

`Router.path` and `Router.params` change together, and `Router.query` changes with
them, so one binding on any of the three sees a consistent set.

`Router.pageStatus` values:

| Value | Meaning |
|-------|---------|
| `Ready` | the matched route's view is built and showing. |
| `Loading` | the view is still being built. A view compiled into the bundle is built synchronously, so a route pointing at one never reports this; a [remote page](remote-pages.md) does, while the edge is being asked for it and the reply has not arrived. |
| `Forbidden` | a route matched, but it declares a `scope` the session lacks. `path` is now `router.fallback` and the fallback's view is showing. The refused path is remembered for [after login](#returning-to-the-page-that-was-refused). |
| `NotFound` | nothing in the route table matched. `path` is now `router.fallback`, the fallback's view is showing, and the query the unmatched path carried is dropped. |
| `Error` | there is no page to show: a compiled-in view failed to load, because it does not compile or because its URL names nothing, or a [remote page](remote-pages.md) could not be delivered, because the edge refused it or the client has no loader to resolve it. A route that declares neither a `view` nor a `remote` in `synqt.yaml` never becomes a page at all: `synqt check` reports it, and `synqt build` refuses to generate it. `Error` also wins over `Forbidden` and `NotFound` when it is the *fallback's* own view that failed, because a broken fallback is the more urgent fact and is what an app has to surface first. |

`Router` is bound as a context property rather than as a registered QML type, so
the value names above are not in scope in QML: `pageStatus` reads there as its
integer value, counting from zero in the order of the table.

### Rendering the current page

`pageComponent` is the member an application actually renders. One `Loader` in
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
parameter that captures. When two routes both match, **the one with more literal
segments wins, whatever order they are declared in**: `/c/summary` beats
`/c/:campaign` even when `/c/:campaign` comes first in `synqt.yaml`. Precedence is a
property of the table, not of the order something happened to emit it in.

An empty segment is not a segment, so `/c` and `/c/` are one route and `synqt check`
[rejects declaring both](project-layout-and-config.md#validation). A query string is
never part of a path: it is split off before matching and arrives in
`Router.query`.

### Deep links, refreshes, and scope changes

The generated client resolves the URL the page was loaded at, at boot, before the
link to the edge opens. A visitor who bookmarked `/c/summer-sale`, or who pressed
refresh on it, lands on that page rather than on the home page. The edge cooperates
by [serving the application shell](security.md#deep-links-and-the-login-resume) for
any path it does not answer itself.

At that moment the session holds only the default scope, because the link to the
edge has not opened yet. A **scope-gated** deep link therefore resolves `Forbidden`
at boot, and is resumed the moment the real scope arrives.

The router re-resolves the current route on every scope change, in both directions:

- **Gaining** scope (a sign-in) promotes a route that was refused, and then replays
  a remembered destination.
- **Losing** scope (a sign-out, or an expired session) evicts the visitor from a
  page they may no longer see, instead of leaving them sitting on it. The address
  bar is corrected with it, so a refresh does not walk straight back into the
  redirect.

Neither is a navigation, so neither adds a history entry.

### Returning to the page that was refused

When a guard refuses a navigation, the router remembers the path (never the query
string, which may carry a token) and replays it once the session can reach it. A
visitor who follows a link to `/admin`, signs in, and holds `admin` afterwards ends
up on `/admin`, not on the home page with no explanation.

The remembered path is cleared by being read, whether or not it turned out to be
usable, so a stale intent cannot steer a later visit. It is **not** cleared by
navigating somewhere else: a visitor bounced off `/admin` who then browses to
`/products` and signs in there is still taken to `/admin`. That is deliberate: the
page they were refused is the one they asked for, and whatever they looked at while
signed out was them waiting to be let in.

The stored path is under the control of whoever put the link in front of the
visitor, so it is validated before anything acts on it. The rules, and why they are
what they are, are in
[deep links and the login resume](security.md#deep-links-and-the-login-resume).

A route guard is a **redirect rule, not a secrecy mechanism**. The client is one
compiled bundle, so every view's QML ships to every visitor; guards steer
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
| `App.applyUpdate()` | action | reload onto the new build. Instant: the shell cache fetched it before raising the signal. |

**If you handle `updateReady`, you own the timing. If you handle nothing, the client
reloads immediately**, on the grounds that an update nobody applies is worse than an
interruption. That default is a mechanism, not a convention: the runtime checks whether
anything is connected to the signal and reloads when nothing is.

Handle it whenever a reload could lose work. `App.onUpdateReady` is an attached
handler, so it reads like a contract's own signal (`Arena.onEaten`) and needs no
`Connections` block:

```qml
App.onUpdateReady: updateBanner.visible = true   // "A new version is ready" / Reload
```

and apply it once the moment is safe:

```qml
Button {
    text: qsTr("Reload")
    onClicked: App.applyUpdate()
}
```

Requires `build.client_cache: service_worker` (the default). Under `http` the signal
never fires: a new build arrives on the next load instead.

## Service: `Caller`

Inside a connect point's slot, `Caller` is whoever invoked it. It is one of two
things, and which one is explicit. This is how an owner authorizes a request
without any ambient global.

| Member | Available when | Type | Description |
|--------|----------------|------|-------------|
| `Caller.isUser` | always | bool | true when the call came from a browser client. Only possible on a web edge connect point. |
| `Caller.isEntity` | always | bool | true when the call came from another entity over a mesh link. |
| `Caller.session` | `isUser` | object | the caller's session record (`id`, `identity`, `scope`). |
| `Caller.identity` | `isUser` | object \| null | the caller's normalized identity (same fields as [`Session.identity`](#client-session)), or `null` if anonymous. |
| `Caller.scope` | `isUser` | string \| list | the caller's scope. |
| `Caller.hasScope(name)` | `isUser` | bool | whether the caller holds `name` (hierarchical where configured). |
| `Caller.setScope(scope)` | `isUser` | action | set the session's scope. Used by the identity flow after login; rotates the session id on privilege change. |
| `Caller.emit<Signal>(...)` | `isUser` | action | emit a contract signal back to **this one caller** (see [targeting](#emitting-a-signal-to-one-caller-versus-all)). |
| `Caller.id` | `isUser` | string | the session id (also `Client.id`). |
| `Caller.entity` | `isEntity` | string | the calling entity's authenticated name, taken from the certificate its mutual-TLS link verified. |

Two authorizations at two boundaries, from the [end-to-end
example](programming-model.md#a-connect-point-implementation-end-to-end): the edge
checks the user, the database checks the calling entity.

```qml
// web/Todo.qml: the edge authorizes a user
function add(text) {
    if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in first."); return }
    Database.items.insert({ text: text.trim(), ownerSub: Caller.identity.sub })
}

// database/Items.qml: the database authorizes the calling entity
function insert(row) {
    if (Caller.entity !== "web") return    // only the edge may write
    Db.exec("INSERT INTO items(text, owner_sub) VALUES(?,?)", [row.text, row.ownerSub])
}
```

!!! warning "Two identity systems, never conflated"
    `Caller.isUser` (a browser session, identified by login and scope) and
    `Caller.isEntity` (a service, identified by certificate) are separate systems.
    `Caller.entity` is certificate-authenticated on every mesh link by default. A
    user-supplied value is never an entity identity. See
    [security](security.md).

Outside a call that originated from a consumer (for example an owner-side timer
mutating shared state), there is no caller. The owner simply writes the Source and
QtRO fans the change out to every consumer.

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
`Caller`; `Client` exists because most edge slots are only ever called by a
browser user and reading `Client` there is clearer than reading `Caller`.

---

## Owner: the generated Source surface

The owner of a connect point implements it against the Source type the contract
generator emits, named `<Contract>Source`. This is the only place authoritative
state is written. For `contract Todo { prop int count; model items(text, author);
signal rejected(string reason); slot add(string text) }` the owner's Source
exposes:

| Surface | From | Description |
|---------|------|-------------|
| `count = n` | `prop count` | assign to push a new value to every consumer. The owner is the only writer; consumers get a read-only mirror. |
| `setItems(rows)` | `model items(...)` | replace the model with `rows`. Only the declared roles cross; any extra field on a row (an owner id, a timestamp) is dropped at the boundary and never serializes to a consumer. |
| `rejected(reason)` | `signal rejected` | emit the signal to **all** consumers of this Source instance. |
| `add(text) { ... }` | `slot add` | the slot body you write; `Caller` is available inside it. |

The `set<Model>` name follows the model name: `model winners(...)` gives
`setWinners(rows)`, `model players(...)` gives `setPlayers(rows)`. Replacing the
rows wholesale is the owner surface today; finer-grained updates are an
optimization behind the same declaration. See the [contract
generator](programming-model.md#contracts-the-shape-of-what-may-cross) for how
each `.syn` construct lowers.

### Emitting a signal to one caller versus all

There are two ways to emit a contract signal, and the difference is the audience:

- **Calling the Source's signal** (`rejected(reason)`) delivers it to every
  consumer of that Source instance. With a `shared` instance that is everyone;
  with a `per_session` instance there is only one consumer, so it is that session.
- **`Caller.emit<Signal>(...)`** (`Caller.emitRejected(reason)`) delivers it to
  the one caller currently in the slot. Use it to answer a specific request on a
  `shared` instance without notifying the others.

For a `per_session` or `per_peer` connect point the two coincide, because the
instance has a single consumer; `Caller.emit<Signal>` is the habit to keep because
it stays correct if the instance later becomes `shared`.

---

## Availability and lifecycle

The framework, not your code, owns each accessor's lifecycle:

- A **scope-gated** `Server.<name>` is acquired only when the session meets the
  connect point's `scope`. Below that scope the Replica is never handed over, so
  its slots cannot be called at all; the gate is enforced at acquisition, not by
  hiding buttons. On a scope upgrade (`Caller.setScope` after login) the newly
  permitted connect points are acquired; on logout they are released.
- Attached signal handlers (`<Contract>.on<Signal>`) fire only while the connect
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
