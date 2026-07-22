# Programming model

This is the model a developer writes against. It defines contracts, connect
points and their ownership, the accessors used to reach across the boundary
(`Server`, entity names, `Caller`, `Session`), sessions and scopes, and cross
entity calls. The goal: writing across a boundary feels like writing one QML
application, while the boundary stays explicit and one directional in trust.

## Contracts: the shape of what may cross

A contract declares the API of one connect point: its live properties, its owner
to consumer signals, its consumer to owner calls, and any live models. A contract
lives in `shared/` and is the single declaration every entity on either end of
the connect point compiles against. SynQt contracts are a friendly surface over
QtRemoteObjects rep files; the build generates the QtRO Source and Replica from
them.

A contract file uses the `.syn` extension. Example, `shared/Todo.syn`:

```syn
// Direction of travel is fixed by the keyword:
//   prop   : owner held value, owner -> consumer updates
//   model  : owner held list, owner -> consumer updates, only listed roles cross
//   signal : owner -> consumer event
//   slot   : consumer -> owner request, the owner decides whether to act
contract Todo {
    prop int count                          // read on the consumer, set on the owner
    model items(text, author, done)         // private row fields never cross
    slot add(string text)                   // returns nothing; fire and forget request
    slot remove(int index)
    slot bool clear()                       // returns a value; becomes an async call
    signal rejected(string reason)          // owner explains a refusal
}
```

Mapping to the QtRO semantics the generated rep encodes:

- `prop` generates a property with push semantics. The consumer gets a getter and
  a generated push request; never a direct setter. The owner is the only writer.
  This is the QtRO READPUSH default, and it is deliberate.
- `model` generates a QtRO MODEL exposing only the named roles. Any other field on
  an owner row is invisible to consumers. On the owner, the generated Source
  provides `set<Model>(rows)` (for `model items(...)`, `setItems(rows)`): it takes
  an array of row objects as the new authoritative model state and replicates only
  the declared roles, so a row may carry extra owner only fields (an owner id, a
  timestamp) that are dropped at the boundary and never serialize to any consumer.
  Replacing the rows wholesale is the owner surface today; finer grained row
  updates are an optimization behind the same declaration.
- `signal` and `slot` map directly: signals run owner to consumer, slots run
  consumer to owner.
- A slot with a return type becomes an asynchronous call on the consumer (a
  pending call that resolves later), because the work happens on the owner. A slot
  with no return type is a one way request.

Contracts may declare plain data records for use in signatures, which compile to
QtRO POD types passed by value:

```syn
record Address(string street, string city, string zip)
```

Why a friendlier surface instead of raw rep files. Rep defaults (push versus read
or write, which roles a model exposes) are exactly the places a mistake becomes a
security hole. The `.syn` surface keeps the safe defaults obvious and emits
correct rep without the developer memorizing rep keywords. The generated rep is
available in the build directory for inspection.

## Connect points: owned by one entity, consumed by others

A contract is a type. A connect point is a named, configured use of that type with
an owner and a set of consumers. A connect point is declared in `synqt.yaml` (full
schema in [project layout and configuration](project-layout-and-config.md#the-synqtyaml-schema)):

```yaml
connect_points:
  - name: todo              # the name consumers use to reach it
    contract: Todo          # which contract from shared/
    owner: web              # the entity that holds the authoritative Source
    consumers: [client]     # the entities allowed to acquire the Replica
    server: web/Todo.qml    # the authoritative implementation (in the owner's folder)
    scope: user             # for browser consumers: minimum session scope
    instance: per_session   # per_session, per_peer, or shared
```

The configurable parts that matter:

- `owner` and `consumers`. The owner holds the authority. The consumers list is an
  allowlist: only those entities may acquire the Replica, and the framework opens
  only the mesh links that this implies. A connect point owned by the database and
  consumed by the web edge is reachable by the edge and nobody else, and is never
  reachable by the browser, because the browser is not a listed consumer and
  cannot physically reach the database anyway.
- `scope` (for browser consumers). The minimum session scope a browser user must
  hold before the framework will acquire the Replica for that client. A user below
  the required scope never gets the object, so cannot call its slots at all.
- `instance`. `shared` means one authoritative Source for all consumers (a public
  feed). `per_session` means one Source per browser session (a private draft).
  `per_peer` means one Source per calling entity (useful when one service serves
  several others and must keep their state separate).

## Reaching a connect point: accessors

How you reach a connect point depends on where your code runs.

From the browser client, the connect points it consumes appear under `Server`,
which is an alias for the web edge this client is attached to:

```qml
// client/TodoView.qml
Label { text: "Items: " + Server.todo.count }          // live property
ListView { model: Server.todo.items }                  // live model
Button { onClicked: Server.todo.add(input.text) }      // a request
Todo.onRejected: reason => banner.show(reason)         // owner explained a refusal
```

From any entity's code, a connect point on another entity appears under that
owner entity's name. For example, inside the web edge's code, the database's
connect points are under `Database`:

```qml
// web/Todo.qml (the edge), calling the database entity
function add(text) {
    if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in first."); return }
    // Persist through the database entity. This is an async cross entity call.
    Database.items.insert({ text: text.trim(), author: Caller.identity.email })
}
```

`Server` is therefore just the well known name for "the edge a browser client
talks to." The general form is `<EntityName>.<connectPoint>`, addressing the
owner by its configured name, capitalized into a QML type like accessor: entity
`database` appears as `Database`, entity `web` as `Web`. (`Server` is the client's
alias for its own edge, whatever that edge entity is named.)

## Handling a connect point's signals

A `Connections` block reacts to a connect point's signals, but it is a lot of
ceremony for the common case:

```qml
Button { onClicked: Server.auth.login(user.text, pass.text) }

Connections {
    target: Server.auth
    function onLoginFailed(reason) { errorPopup.text = reason; errorPopup.open() }
}
```

The contract already declares every signal, so SynQt generates, for each contract,
an attached handler type named after the contract. Write `<Contract>.on<Signal>`
on any element to react to that connect point's signals: no `target`, no `function`
wrapper, and the handler names are checked against the contract at compile time.
Given `contract Auth { slot login(...); signal loginFailed(string reason); signal loggedIn() }`,
the block above becomes two lines:

```qml
Button { onClicked: Server.auth.login(user.text, pass.text) }

Auth.onLoginFailed: reason => { errorPopup.text = reason; errorPopup.open() }
Auth.onLoggedIn:    () => Router.go("/home")
```

The attached type binds to the connect point the current entity consumes for that
contract. When an entity consumes more than one connect point of the same contract,
name which one with `<Contract>.point`:

```qml
Auth.point: "adminAuth"
Auth.onLoginFailed: reason => banner.show(reason)
```

The same shorthand works on the service side, for the signals of a connect point an
entity consumes from another entity. Inside the web edge, reacting to the database's
`Ledger` signals, `Connections { target: Database.ledger; function onWinnersChanged() {...} }`
collapses to:

```qml
Ledger.onWinnersChanged: hall.refresh()
```

Handlers fire only while the connect point is live; before it is acquired (a browser
below the required scope, or a link still connecting) they simply do not fire, and
they resume on reconnect, because the framework owns the replica's lifecycle.

Why the type is named after the contract rather than a single `Self` attached to
everything: a single object could not be checked at compile time (its set of signals
would depend on which connect point you meant), and it could not disambiguate a view
that touches several connect points at once. The contract name gives the compiler an
exact signal set to verify each `on<Signal>` against. `Connections` stays available
and remains the right tool when the target is dynamic or is not a connect point.

## Reaching the caller: the `Caller` accessor

Inside a connect point's slot, the framework exposes whoever invoked it through
`Caller`. This is how an owner reaches the entity that called it, without a single
global object. The caller is one of two things.

- A browser user session, when the call came from a client entity (only possible
  on a web edge connect point). `Caller.isUser` is true. `Caller.session`,
  `Caller.identity`, `Caller.scope`, `Caller.hasScope(name)`, and
  `Caller.emit<Signal>(...)` (emit a contract signal back to this one client) are
  available. `Caller.setScope(...)` is used by the identity flow after login.
- Another entity, when the call came over a mesh link. `Caller.isEntity` is true
  and `Caller.entity` is the calling entity's authenticated name, taken from the
  certificate the link's mutual TLS verified; mesh links are mutual TLS by default
  on one host (over loopback) and across hosts alike. (On an opt in local socket
  link the name is trusted by colocation instead; see [security](security.md).)
  The owner authorizes by entity: for example a database slot can require
  `Caller.entity === "web"`.

`Client` remains available on web edge connect points as a convenience alias for
`Caller` when the caller is a browser user, so existing edge code reads
directly (`Client.hasScope`, `Client.identity`, `Client.emit<Signal>`, and
`Client.id` for the session id). The general mechanism is `Caller`.

Outside a call that originated from a consumer (for example an owner side timer
mutating shared state), there is no caller; the owner simply mutates the Source and
lets QtRO fan the change out to all consumers.

## A connect point implementation, end to end

`web/Todo.qml`, the authoritative Source on the edge, authorizing the user and
delegating persistence to the database entity:

```qml
import QtQuick
import SynQt

TodoSource {
    id: todo

    function add(text) {
        // The edge authorizes the user.
        if (!Caller.hasScope("user")) {
            Caller.emitRejected("You must sign in to add items.")
            return
        }
        const clean = ("" + text).trim()
        if (clean.length === 0 || clean.length > 280) {
            Caller.emitRejected("Item must be 1 to 280 characters.")
            return
        }
        // Persist via the database entity (async cross entity call).
        // The database will authorize that the caller is the edge.
        Database.items.insert({ text: clean, author: Caller.identity.email, ownerSub: Caller.identity.sub })
    }
}
```

`database/Items.qml`, the authoritative Source on the database entity, authorizing
the calling entity:

```qml
import QtQuick
import SynQt

ItemsSource {
    id: items

    function insert(row) {
        // The database authorizes the calling entity, not a user.
        if (Caller.entity !== "web") {
            return   // refuse calls from any entity other than the edge
        }
        Db.exec("INSERT INTO items(text, author, owner_sub) VALUES(?,?,?)",
                [row.text, row.author, row.ownerSub])   // see docs/entities.md for the Db helper
    }
}
```

Two authorizations at two boundaries: the edge checks the user, the database
checks the calling entity. This is the mesh in microcosm.

## Sessions and scopes (browser users)

A session is the web edge's record of one authenticated or anonymous browser
connection. It holds the identity and the scope. Scopes are declared in
`synqt.yaml` so connect point gates and identity mapping share one vocabulary:

```yaml
scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous
```

Hierarchical scopes let `hasScope("user")` be true for a higher scope. Projects
that want set based scopes set `hierarchical: false` and assign explicit scope
sets. Hierarchical is the default because it is the least surprising.

On the client, session state is read only through `Session`:

- `Session.scope`, `Session.hasScope(name)`.
- `Session.state`: `connecting`, `connected`, `reconnecting`, `denied`, `offline`.
- `Session.identity`: the authenticated identity, or null when anonymous.
- `Session.login()` and `Session.logout()`.

Client side scope checks (hiding a button) are user experience only. They are
never the security boundary. Every privileged action is checked again on the
owner, inside the slot, against `Caller`. This duplication is intentional and is
restated in the security document.

## Route guards (which client views are reachable)

The client is a single compiled bundle, so all of its QML ships to every visitor.
That is fine: shipping the structure of a page is not shipping the data behind it,
and data only arrives through scope gated connect points. Route guards steer
navigation:

```yaml
router:
  fallback: /

routes:
  - path: /
    view: Home.qml

  - path: /c/:campaign      # a path parameter, read in QML as Router.params.campaign
    view: Campaign.qml

  - path: /admin
    view: Admin.qml
    scope: admin            # below this scope, the router redirects to fallback
```

Each route is a real URL, so a visitor can bookmark it, share it, and refresh on it.
`Router.pageComponent` is what a single `Loader` in `Main.qml` renders, and
`Router.path`, `Router.params`, and `Router.query` are what a view binds to. The
members are listed in the [runtime API reference](runtime-api.md#client-router),
and the keys in
[configuration](project-layout-and-config.md#router-and-routes-client-navigation).

A guard is a redirect rule, not a secrecy mechanism. The privileged screen still
renders nothing useful without privileged connect points, which the edge refuses
to provide to an under scoped session, and which often resolve through services
the browser cannot reach at all.

## Connection lifecycle and offline behavior

Each link uses a QtRO heartbeat so a dropped connection is noticed promptly rather
than only on the next send (QtRO disables the heartbeat by default; SynQt enables
it). On a browser disconnect, `Session.state` becomes `reconnecting` and the
client retries with capped exponential backoff; replicas report not ready and QML
can show cached values or an offline banner. A session the edge rejects (expired or
revoked credential) moves to `denied`, and the client routes back through login.
Service to service links reconnect the same way; an entity that loses a consumed
connect point reports it as not ready and retries, so a transient database restart
does not crash the edge.

## The mental model

- You declare what may cross in a contract. The defaults make the safe choice.
- You name each connect point, give it an owner and a consumer allowlist, and (for
  browser consumers) a scope. The framework wires the links and authenticates them.
- Consumer code reads `Server.<name>` (browser) or `<Entity>.<name>` (services) and
  calls slots, treating every call as a request.
- Owner code implements the slots, checks `Caller` (a user session or a calling
  entity) for authorization, and is the only writer of authoritative state.
- The framework moves the bytes, reconnects, authenticates every link, and keeps
  per session and per peer authoritative state separate when you ask for it.

There is no single Server object and no single Client object to subclass. There
are entities, the connect points they own and consume, the callers that reach
them, and the contracts that define exactly what may travel.
