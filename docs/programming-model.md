<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Programming model

This page describes the model you write code against: contracts, connect points and
their ownership, the accessors that reach across a boundary (`Server`, entity names,
`Caller`, `Session`), sessions and scopes, and calls between entities. Code across a
boundary should read like one QML application, while the boundary stays explicit and
trust flows one way.

## Contracts: the shape of what may cross

A contract declares the API of one connect point: its live properties, its signals from
owner to consumers, its calls from consumers to owner, and any live models. Every entity
at either end compiles against this one declaration.

You write it on the connect point itself, in `synqt.yaml`, in an `export:` block, so a
point and its shape live in one place:

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    # Direction of travel is fixed by the keyword:
    #   prop   : owner held value, owner -> consumer updates
    #   model  : owner held list, owner -> consumer updates, only listed roles cross
    #   signal : owner -> consumer event
    #   slot   : consumer -> owner request, the owner decides whether to act
    export: |
      prop int count                        // read on the consumer, set on the owner
      model items(string[280] text, string[80] author, bool done)  // private fields never cross
      slot add(string[280] text)            // returns nothing; fire and forget request
      slot remove(int index)
      slot bool clear()                     // returns a value; becomes an async call
      signal rejected(string[120] reason)   // owner explains a refusal
```

The owner names the contract: the exported type is the owner's name, capitalized. An
`edge` entity exports `Edge`, the QML type at the root of the owner's Source file and the
name a consumer's attached handlers use. Contracts sit on top of QtRemoteObjects rep
files. The build writes one `.syn` per point under `generated/` and generates the QtRO
Source and Replica from it. Never edit that file; edit the `export:` block, which
rewrites it.

### Exporting by name

The owner already defines most members. `web/edge/Edge.qml` binds a property,
implements a function, publishes rows, so the kind and often the type are already
written there. Name the member, and `synqt` reads the rest from the owner:

```yaml
    export: |
      count                                 // prop int, read off the owner
      slot add(string[280] text)
      signal rejected(string[120] reason)
```

A name resolves only when the owner is unambiguous. Otherwise `synqt check` refuses the
line and prints what it read, for you to correct and paste:

```
error: connect point 'edge': 'add' is exported by name, and web/edge/Edge.qml does not
say what type it is. Write it out: 'slot add(var text)' is what was read, with whatever
it left open to fill in
```

A type it could not read stays a question; it never becomes `var` in the contract. A guess
is a fine starting point for a person and a bad thing to put on a wire. In practice, a
property bound to the entity's own singleton resolves (the declaration is one file away),
a slot's parameter types usually do not, and a model's roles never do, because the rows
are built elsewhere.

### What the owner has to answer for

Whether a member is only named or written in full, `synqt check` compares it with the
owner's Source. These are errors:

- **A member the owner does not implement.** A slot with no QML function behind it fails
  silently: the call returns a default and nothing says why.
- **A member exported as one kind and written as another,** such as `prop bidRejected`
  against `Caller.emitBidRejected(...)`.
- **A property exported with a type the owner clearly contradicts.** `int` against `real`
  is fine, since JavaScript has one numeric type. `string` against `int` is not.

The check matches the shape of the QML instead of compiling it, so it reports what clearly
disagrees and stays quiet where it cannot tell. `synqt infer` prints the same reading, and
[`synqt infer --write`](build-system-and-cli.md) fills an empty `export:` from it.

How each member maps to the QtRO semantics in the generated rep:

- **`prop`** generates a property with push semantics (the QtRO READPUSH default). The
  consumer gets a getter and a generated push request, never a direct setter. Only the
  owner writes it.
- **`model`** generates a QtRO MODEL that exposes only the named roles; consumers never see
  any other field of an owner's row. The generated Source publishes rows two ways: bind
  `<model>Rows` to where the rows live (for `model items(...)`, `itemsRows: Edge.items`)
  so every change republishes, or call `set<Model>(rows)` (`setItems(rows)`) for rows
  that arrive from an event. Both take an array of row objects as the new authoritative
  state and replicate only the declared roles. A row may carry extra fields for the owner
  (an owner id, a timestamp); they are dropped at the boundary and never reach a
  consumer.
  Each role has a declared type. A value that does not convert refuses the publish, with a
  message naming the model, the role and the row, so a row that does not match the
  contract never reaches a consumer. Declare a role `var` only if it really can hold
  anything. Today the owner replaces rows wholesale; finer row updates would be an
  optimization behind the same declaration.
- **A model flows from owner to consumers only.** A consumer cannot write to it, even though
  the underlying Qt type has `setData`. To change owner state, a consumer calls a slot,
  where `Caller` exists and the owner decides.
- **`signal` and `slot`** map directly: signals run from owner to consumer, slots from
  consumer to owner.
- **A slot with a return type** is an asynchronous call on the consumer, resolved later,
  because the work runs on the owner. A slot with no return type is a one way request.
  On the consumer it reads `Server.clear().then(ok => ...)`. Attach the handler to the
  call, as here, instead of storing the promise for a later frame: a promise is retired
  once it has settled and delivered. When the link drops during a call, the call is
  rejected when the link returns, so a chained `.catchError(reason => ...)` runs instead
  of waiting forever.

An `export:` block may also declare plain data records for use in signatures. They compile
to QtRO POD types passed by value:

```yaml
    export: |
      record Address(string[120] street, string[80] city, string[16] zip)
      slot deliver(Address to)
```

### The types a contract can name

A value crossing a connect point is read from QML on the owner and handed to QML on the
consumer, so a contract uses the
[built-in QML value types](https://doc.qt.io/qt-6/qtqml-typesystem-valuetypes.html) and
nothing else. Each name means what the QML documentation says.

| written | on the wire | notes |
|---------|-------------|-------|
| `bool` | `bool` | |
| `date` | `QDateTime` | |
| `double` | `double` | |
| `int` | `int` | |
| `list` | `QVariantList` | a list of `var`, and a typed list of rows is a `model` |
| `real` | `double` | |
| `string` | `QString` | |
| `url` | `QUrl` | |
| `var` | `QVariant` | anything, checked by nobody |
| `variant` | `QVariant` | the older spelling of `var` |

The four types with no natural limit can take a size in brackets:

| written | bounds |
|---------|--------|
| `string[64]` | at most 64 characters |
| `url[200]` | at most 200 characters |
| `list[100]` | at most 100 elements |
| `var[4096]` | at most 4096 bytes once serialized |

The owner's boundary enforces a bound wherever a value crosses: an assignment to a
bounded `prop`, a role on a published row, an argument arriving on a `slot`, an argument
leaving on a `signal`. A value that does not fit is refused and named in a warning, never
truncated: a silently shortened name or a dropped tail is the bug a bound prevents. Bound
the fields that reach a database column, a filename or a rendered label, and leave the
rest unbounded.

### The names a contract can use

Every name in a contract becomes a C++ name in the generated code, and the generated
classes already have members of their own. The build stops, naming the member, on:

- a C++ keyword or one of the words Qt defines as a macro (`class`, `default`, `new`,
  `emit`, `signals`) as any name;
- a member, parameter, role or field name that begins with `synqt`, the prefix of every
  name the generator makes up;
- a member called `ready`, `data`, `objectName`, `destroyed`, `deleteLater`, `parent` or
  `children`, which already mean something on the object a consumer reaches (`Server.ready`
  is the framework's own, see [the runtime API](runtime-api.md));
- a member named like one the generator derives from another member: `setCount` or
  `countChanged` beside `prop int count`, `itemsRows`, `setItems` or `itemsChanged` beside
  `model items(...)`, and `emitRejected` beside `signal rejected(...)`;
- one name twice in a parameter, role or field list;
- a slot with more than 10 parameters, or a signal with more than 8. Group the rest into a
  `record` and send that.

SynQt uses the `export:` block instead of raw rep files because rep's defaults (push
versus read-write, which roles a model exposes) are where a mistake becomes a security
hole. The block makes the safe defaults obvious and emits correct rep, so you need not
memorize rep keywords. The generated rep is in the build directory if you want to
inspect it.

## Connect points: owned by one entity, consumed by others

An entity has one connect point: the surface it exports, with an owner and a set of
consumers. The point has no name of its own; the owner is its name. Consumers reach it as
the owner's name capitalized, which is also its contract's name, and the file that
implements it is that name plus `.qml`. Declare it in `synqt.yaml` (full schema in
[project layout and configuration](project-layout-and-config.md#the-synqtyaml-schema)):

```yaml
connect_points:
  - owner: edge                   # the entity that holds the authoritative Source
    consumers: [app]              # the entities allowed to acquire the Replica
    server: web/edge/Edge.qml     # the authoritative implementation
    scope: user                   # for browser consumers: minimum session scope
    export: |                     # what may cross it, and nothing else does
      prop int count
```

The keys that matter:

- **`owner` and `consumers`.** The owner holds the authority. The consumer list is an
  allowlist: only those entities may acquire the Replica, and the framework opens only
  the mesh links it implies. A point the database owns and the web edge consumes is
  reachable by the edge and nobody else. The browser can never reach it: it is not a
  listed consumer, and it cannot reach the database anyway.
- **`scope`** (for browser consumers). The minimum session scope a browser user needs
  before the framework acquires the Replica for them. A user below that scope never gets
  the object, so cannot call its slots.
- **`export`.** What may cross, written on the point. The type it becomes is the owner's
  name capitalized: `owner: edge` exports `Edge`. Nothing names it separately, and it has
  no suffix.
- **`server`.** The file that implements the connect point. Its root element is the
  contract, so `web/edge/Edge.qml` opens with `Edge { ... }`. That file is the entity, so
  `server` defaults to the entity's own file and most points never set it. Both ends of a
  contract are QML types with the same name, and they never meet, because an entity may
  not consume its own connect point. In the owner's binary, `Edge` is the owner side. In a
  consumer's, it is the consumer side, and the attached handler type for
  [a connect point's signals](#handling-a-connect-points-signals). The file tells you
  which one you are reading. A Source is the `server:` of a connect point its entity
  owns.

### Gating one member: `<scope>`

`scope:` on the point is all or nothing: a visitor below it acquires none of the point.
That suits a point whose whole content is for one audience. An owner that serves a public
page and an admin surface needs more precision, so write the scope on the member:

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    scope: moderator                # the default for every member below
    export: |
      prop string[80] headline
      model catalogue(string[64] sku, real price)

      <admin> slot restock(string[64] sku, int count)
      <admin> model auditLog(string[200] line)
```

A member with no gate inherits the point's `scope:`, so the block above means what it
looks like: moderators get the headline and the catalogue, and admins also get the two
gated members. With the default hierarchical scopes, a higher scope satisfies a lower one,
so admin reaches everything. With `scopes.hierarchical: false`, a caller holds exactly one
scope, and a member for two scopes names both: `<admin,auditor>`.

The point above belongs to the edge, which is where the hierarchy lives. On a point a
service owns, the same gate is an exact match on the scope name in the forwarded session,
so name every scope it is meant for: a service does not receive the vocabulary from its
caller. See [scope down the chain](runtime-api.md#scope-down-the-chain).

The gate controls what crosses, not what is declared. The member stays in the contract, so
a consumer's `Server.storefront` has an `auditLog` model either way. For a caller without
the scope, it is never seeded, never followed and never sent: the rows are never delivered,
rather than delivered and hidden. A gated `slot` is refused before the owner's QML sees the
call, and a gated `signal` is not delivered.

The gate follows the session, not the connection. A visitor who signs in sees what they
are now entitled to without reloading, and a visitor who loses a scope has the gated
members withdrawn from their replica.

`synqt check` refuses two gates that look like protection but are not. A scope missing from
`scopes.order` can never be held by anyone. A gate on a point no client consumes refuses
every caller, since a scope belongs to a user's session and a calling entity has none. To
restrict a member between services, check `Caller.entity` in the slot instead.

### Recording a call's values: `capture`

When a project has a [monitor entity](monitoring.md), every slot call that crosses a link
is recorded: which member, whether a person or an entity called it, how many arguments,
how long it took, and which check refused it, if any. The arguments themselves are not
recorded, because they are what somebody typed.

To keep a member's values, say so on the member:

```yaml
    export: |
      slot capture placeBid(int amount)
```

Now the record of a call to `placeBid` includes `amount`. You set it per member, never per
contract or entity. An operator chasing a refused bid wants to know the amount, but nobody
wants a monitor that quietly collects every value the system handles. A record outlives
its session, is read by people it is not about, and goes wherever an operator sends it, so
what it holds must be a decision, not a default.

`synqt check` refuses `capture` on a member whose arguments carry an identity (`sub`,
`email`, `login`, directly or inside a `record`), because that would make the operations
record a second copy of the identity store. If you want that anyway, say so once, at the
top of `synqt.yaml`:

```yaml
monitoring:
  capture_identity: acknowledged
```

A captured value has the same limit as every other attribute of a record: 512 characters.
Longer text is cut, and a `list`, `var` or `record` that serializes to more is replaced by
a note saying how much was dropped. Capture only members with small values; bounding their
arguments in the contract (`string[80]`, `list[20]`) keeps every capture whole.

`capture` is not a reserved word. A slot may still be named `capture`; what follows the
word tells the compiler which one you mean.

### Handing callers on: `behind:`

Member scopes decide what crosses; `behind:` decides who answers. A web edge can own a
point it does not implement and hand each caller to the entity that serves their scope:

```yaml
  - name: gate
    owner: gate                     # a web_edge
    consumers: [app]
    behind:
      anonymous: lobby
      admin: backoffice
    export: |
      prop string[80] headline
      <admin> slot restock(string[64] sku, int count)
```

The edge keeps what only it can keep, the session and the sign-in, and holds none of the
data. The browser writes `Server.gate` whoever answers. Each entity behind the front owns
an ordinary connect point that the front consumes, and `synqt check` keeps the two in
step: a tier carries exactly the members the front offers its callers, no more and no
fewer.

Callers of one scope and no other reach an entity behind a front, so it authorizes on
`Caller` and never asks about scope. Nothing enforces that at run time because nothing
has to. And with a tier per process, an admin surface's rows never exist in the process
serving anonymous visitors.

The tier that answers a caller follows their scope for the life of the connection, not
only at accept time. A `Caller.setScope` on a live connection points the front at the tier
for the new scope (or withdraws it, if no tier serves that scope), so a demoted admin loses
the backoffice entity at the moment of demotion, not at their next reload. The same happens
when the mesh link to a tier reconnects: the new Replica takes over for every browser
already connected.

A scope with no line of its own goes to the highest tier at or below the caller's scope, so
`anonymous` and `admin` alone still serve a moderator (from the anonymous tier). With set
based scopes there is no order to fall back along, so a scope nobody named is served by
nobody.

A front cannot answer a slot that returns a value. It forwards the call over the mesh, and
the reply arrives after the slot has returned. So `synqt check` refuses a returning slot on
a fronted point and suggests `Caller.emit<Signal>`, which is how a slow answer reaches the
caller in any case.

## How many of an entity there are: `shared`

Read a system as chains. Every chain starts at a browser, which is one person and never
shared, then the edge it connects to, then whatever the edge reaches. `shared:` is each
entity's answer to how many of it exist along that chain:

```yaml
entities:
  - name: app
    type: client            # one browser, so never shared, and it cannot say otherwise

  - name: edge
    type: web_edge
    shared: false           # a Source of its own for each session

  - name: books
    type: relational        # shared: true is the default
```

**`shared: true`** is one Source for the whole entity. Every caller acquires a mirror of it,
so all see the same props and rows, and each slot still runs with that caller's `Caller`.
Because each caller has its own mirror, `Caller.hasScope(...)` still gates,
`Caller.entity` still names the calling entity, and `Caller.emit<Signal>` still reaches
only that caller. Use it for what everybody sees: an auction, a leaderboard, the live
state of a game.

**`shared: false`** is one Source per caller, holding only that caller's state. A browser
caller is a session, so a second tab continues where the first left off, and a private
window gets its own. A mesh caller is the calling entity, so each consuming entity gets
its own. Use it for a draft, a half-filled form, or one player's slice of a world.

`shared:` belongs to the entity, not the connect point: an entity is either one thing
everybody reaches or one thing per caller, never both. `synqt check` refuses a point that
writes `instance:` and names the entity to put `shared:` on instead.

Two consequences:

- **The entity's own `pragma Shared` file has one instance either way.** It is the entity
  itself, not a caller's view of it, so shared state lives there when the entity is not
  shared. An unshared edge with a public feed keeps the feed there, and each session's
  Source publishes it.
- **On a shared entity, `Caller` is whoever is calling right now.** Read it in the slot. If
  the work finishes later, copy what you need to a local first
  (`const who = Caller.session`), because `Caller` will have moved on to the next caller.
  Bindings need no local: `Caller`'s properties notify when they change, so
  `text: Caller.identity.name` follows the caller being served. On an unshared entity,
  each caller has its own Source, and its `Caller` never changes.

Either way, a Source holds live state, not storage. A per caller Source lives while that
caller has at least one link open and disappears when the last one closes. Keep anything
that must survive the last tab closing in the singleton or behind a persistence connect
point.

## Reaching a connect point: accessors

How you reach a connect point depends on where your code runs.

In the browser client, the connect point it consumes is `Server`, an alias for the web edge
the client is attached to:

```qml
// client/app/TodoView.qml
Label { text: "Items: " + Server.count }          // live property
ListView { model: Server.items }                  // live model
Button { onClicked: Server.add(input.text) }      // a request
Edge.onRejected: reason => banner.show(reason)   // owner explained a refusal
```

In an entity's code, another entity's connect point appears under that owner's name. For
example, in the web edge's code, the database's connect point is `Store`:

```qml
// web/edge/Edge.qml (the edge), calling the store entity
function add(text) {
    if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in first."); return }
    // Persist through the database entity. This is an async cross entity call.
    Store.insert({ text: text.trim(), author: Caller.identity.email })
}
```

So `Server` means "the edge this browser client talks to". The general form is
`<EntityName>.<member>`: the owner's configured name, capitalized like a QML type. Entity
`store` appears as `Store`, entity `edge` as `Edge`. There is no second level, because an
entity has one connect point. (`Server` is the client's alias for its edge, whatever the
edge entity is named.)

## Handling a connect point's signals

A `Connections` block can react to a connect point's signals, but it is verbose for the
common case:

```qml
Button { onClicked: Server.login(user.text, pass.text) }

Connections {
    target: Server
    function onLoginFailed(reason) { errorPopup.text = reason; errorPopup.open() }
}
```

The contract already declares every signal, so SynQt generates an attached handler type
per contract, named after it. Write `<Contract>.on<Signal>` on any element to react to that
connect point's signals, with no `target` and no `function` wrapper; the compiler checks
the handler names against the contract. For an edge that exports `slot login(...)`,
`signal loginFailed(string reason)` and `signal loggedIn()`, the block above becomes two
lines:

```qml
Button { onClicked: Server.login(user.text, pass.text) }

Edge.onLoginFailed: reason => { errorPopup.text = reason; errorPopup.open() }
Edge.onLoggedIn:    () => Router.go("/home")
```

The attached type binds to the connect point this entity consumes for that contract. There
is only ever one, because a contract belongs to an owner, and an owner has one point.

The same shorthand works on the service side, for signals of a connect point an entity
consumes from another entity. In the web edge, reacting to the books entity's signals,
`Connections { target: Books; function onWinnersChanged() {...} }` becomes:

```qml
Books.onWinnersChanged: hall.refresh()
```

Handlers fire only while the connect point is live. Before it is acquired (a browser below
the required scope, or a link still connecting) they do not fire, and they resume on
reconnect, because the framework manages the replica's lifecycle.

The type is named after the contract, not a single generic attached type, because a generic
type could not be checked at compile time (its signals would depend on which owner you
meant) and could not tell apart two entities reached from one view. The contract name gives
the compiler an exact set of signals to check each `on<Signal>` against. `Connections`
remains the right tool when the target is dynamic or is not a connect point.

## Reaching the caller: the `Caller` accessor

Inside a connect point's slot, `Caller` is whoever made the call. It is one of two
things:

- **A browser user session,** when the call came from a client entity (possible only on a
  web edge's connect point). `Caller.isUser` is true, and `Caller.session`,
  `Caller.identity`, `Caller.scope`, `Caller.hasScope(name)` and `Caller.emit<Signal>(...)`
  (send a contract signal to this one client) are available. The identity flow uses
  `Caller.setScope(...)` after login.
- **Another entity,** when the call came over a mesh link. `Caller.isEntity` is true, and
  `Caller.entity` is the calling entity's authenticated name, taken from the certificate
  the link's mutual TLS verified. Mesh links use mutual TLS by default, over loopback on
  one host and across hosts. (On an opt in local socket link, the name is trusted by
  colocation instead, and `Caller.isEntityVerified` is false; see
  [security](security.md).) The owner authorizes by entity; for example, a store slot can
  require `Caller.entity === "edge"`.

A calling entity usually acts for somebody, and the framework passes that along: a connect
point a service consumes carries the session the caller acts for, so `Caller.identity` and
`Caller.hasScope(...)` still work on an entity the browser can never reach.
`Caller.isEntity` stays true, because the caller is still that entity; it now also has a
person behind it, as asserted by the entity its certificate identified. There, `hasScope`
is an exact match on the session's scope name, because the hierarchy is configured on the
edge and a caller does not hand a service its vocabulary. See
[the session down the chain](runtime-api.md#the-session-down-the-chain) for the rules,
limits and exactly what travels.

On a web edge's connect points, `Client` is an alias for `Caller` when the caller is a
browser user (`Client.hasScope`, `Client.identity`, `Client.emit<Signal>`, and `Client.id`
for the session key). `Caller` is the general mechanism.

Outside a call from a consumer (a timer on the owner, or the entity's own singleton) there
is no caller, and `Caller` is not in scope. `synqt check` refuses a file outside a Source
that mentions it, because an authorization line that can never run still looks like one to
every reviewer.

## A connect point implementation, end to end

`web/edge/Edge.qml`, the authoritative Source on the edge. It authorizes the user and
leaves storage to the database entity:

```qml
import SynQt

Edge {
    id: todo

    // `add` is exported as `<user> slot add(...)`, so a signed-out caller does not have
    // it and never reaches this function. What is left is the judgement the topology
    // cannot make: whether this particular text is acceptable.
    function add(text) {
        const clean = ("" + text).trim()
        if (clean.length === 0 || clean.length > 280) {
            Caller.emitRejected("Item must be 1 to 280 characters.")
            return
        }
        // Persist via the database entity (async cross entity call). The database lists
        // the edge as its only consumer, so this is the only link into it that exists.
        Store.insert({ text: clean, author: Caller.identity.email,
                       ownerSub: Caller.identity.sub })
    }
}
```

`db/relational/store/Store.qml`, the authoritative Source on the database entity. It checks
nobody, because its consumer list has one name.

```qml
import SynQt

Store {
    id: items

    function insert(row) {
        Db.exec("INSERT INTO items(text, author, owner_sub) VALUES(?,?,?)",
                [row.text, row.author, row.ownerSub])   // see docs/entities.md for the Db helper
    }
}
```

The scope on the member decides who may ask the edge. The edge decides whether to ask the
database. The consumer list decides who may ask the database at all. Add `Caller.entity`
to that last decision when an owner has two consumers and one may do less; with only one
consumer, it repeats what the topology already guarantees.

Test this slot, especially the cases no UI offers: the signed-out visitor and the
oversized item. [Testing your app](testing.md) shows how, in QML, against the real
`Caller`.

## Sessions and scopes (browser users)

A session is the web edge's record of one browser, signed in or anonymous. It holds the
identity and the scope. Scopes are declared in `synqt.yaml`, so connect point gates and
identity mapping share one vocabulary:

```yaml
scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous
```

With hierarchical scopes, `hasScope("user")` is true for any higher scope too. For set
based scopes, set `hierarchical: false`; every check is then an exact match on the
session's one scope. Hierarchical is the default because it surprises least.

On the client, session state is read only through `Session`:

- `Session.scope`, `Session.hasScope(name)`.
- `Session.state`: `offline`, `connecting`, `connected`, `reconnecting`.
- `Session.identity`: the authenticated identity, or null when anonymous.
- `Session.login()` and `Session.logout()`.

Scope checks on the client (hiding a button) only improve the user experience; they are
never the security boundary. The owner checks every privileged action again, in the slot,
against `Caller`. [Security](security.md) explains why the check exists in both places.

## Route guards (which client views are reachable)

The client is one compiled bundle, so all of its QML ships to every visitor. A page's
structure carries none of its data, which arrives only through scope gated connect points.
Route guards steer navigation:

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

Each route is a real URL that a visitor can bookmark, share and refresh. A single `Loader`
in `Main.qml` renders `Router.pageComponent`, and views bind to `Router.path`,
`Router.params` and `Router.query`. The members are listed in the
[runtime API reference](runtime-api.md#client-router), and the keys in
[configuration](project-layout-and-config.md#router-and-routes-client-navigation).

A guard redirects; it keeps nothing secret. A privileged screen shows nothing useful
without its privileged connect points, which the edge refuses to a session below their
scope, and which often reach services the browser cannot reach at all.

A route is either compiled into the client bundle with `view:`, or delivered by the web edge
on demand with `remote:`. A `remote:` route names a QML file the edge holds and sends over
the same `wss` link when the visitor navigates, so a rarely used or often changed page
stays out of the bundle and changes without a client rebuild. Unlike a compiled-in view,
a delivered page's `scope` is enforced on the edge before delivery, so its markup never
reaches a visitor below that scope. The data it reads is still governed by the connect
point's own scope. See [remote pages](remote-pages.md).

## Connection lifecycle and offline behavior

Each link runs a QtRO heartbeat, so a dropped connection is noticed quickly, not only on
the next send (QtRO disables the heartbeat by default; SynQt turns it on). When the
browser disconnects, `Session.state` becomes `reconnecting` and the client retries with
capped exponential backoff. Replicas report not ready, and QML can show cached values or
an offline banner. A session the edge rejects (an expired or revoked credential) looks
the same, because the browser does not say why a handshake failed; an app detects it when
`Session.isAuthenticated` goes false and its scope-gated replicas are released.

Links between services reconnect the same way. An entity that loses a consumed connect
point reports it not ready and retries, so a brief database restart does not crash the
edge.

## The mental model

- **Contract:** you declare what may cross, and the defaults are the safe choice.
- **Connect point:** you give it an owner (which is also its name), a consumer allowlist,
  and, for browser consumers, a scope. The framework wires and authenticates the links.
- **Consumer code** reads `Server.<member>` (browser) or `<Entity>.<member>` (services) and
  calls slots, treating every call as a request.
- **Owner code** implements the slots, checks `Caller` (a user session or a calling
  entity), and is the only writer of authoritative state.
- **The framework** moves the bytes, reconnects, authenticates every link, and keeps state
  separate per session or per calling entity when you ask.

There is no single Server object or Client object to subclass. There are entities, the
connect points they own and consume, the callers that reach them, and the contracts that
define what may travel.
