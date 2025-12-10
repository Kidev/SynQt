# Programming model

This is the model a developer writes against. It defines contracts, connect
points and their ownership, the accessors used to reach across the boundary
(`Server`, entity names, `Caller`, `Session`), sessions and scopes, and cross
entity calls. The goal is that writing across a boundary feels like writing one QML
application, while the boundary stays explicit and one directional in trust.

## Contracts: the shape of what may cross

A contract declares the API of one connect point: its live properties, its owner
to consumer signals, its consumer to owner calls, and any live models. It is the single
declaration every entity on either end of the connect point compiles against.

It is written on the connect point itself, in `synqt.yaml`, in an `export:` block. A
point and the shape of what crosses it are one thing, so there is one place to read and
one thing to name:

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

The owner names the contract, so nothing else has to: the type it exports is the owner
capitalized plus `Contract`. An `edge` entity exports `EdgeContract`, which is the QML type
the owner's Source file is rooted at and the name a consumer's attached handlers use. SynQt
contracts are a friendly surface over QtRemoteObjects rep files; the build writes one
`.syn` per point under `generated/` and generates the QtRO Source and Replica from it.
Nobody edits that file; editing the `export:` block rewrites it.

### Exporting by name

The owner already says what most of these are. `web/edge/EdgeContract.qml` binds a property,
implements a function, publishes rows, so the kind and often the type are written down
there. Name the member and `synqt` reads the rest from the owner:

```yaml
    export: |
      count                                 // prop int, read off the owner
      slot add(string[280] text)
      signal rejected(string[120] reason)
```

A name resolves only when the owner is unambiguous about it. Where it is not, `synqt
check` refuses the line and prints what it read, for you to correct and paste:

```
error: connect point 'todo': 'add' is exported by name, and web/edge/EdgeContract.qml does not
say what type it is. Write it out: 'slot add(var text)' is what was read, with whatever
it left open to fill in
```

That is the rule rather than an accident. A guess is a fine starting point for a person
and a bad thing to put on a wire, so a type nobody read stays a question rather than
becoming `var` in a contract. In practice a property bound to the entity's own singleton
resolves (the declaration is one file away), a slot's parameter types usually do not, and
a model's roles never do, because the rows are built somewhere else.

### What the owner has to answer for

Whether a member is named or written out in full, `synqt check` holds it to the owner's
Source:

- a member nothing in the owner implements is an error. A slot with no QML function
  behind it is the one that always breaks quietly, because the call returns a default and
  nothing says why.
- a member exported as one kind and written as another is an error (`prop bidRejected`
  against a `Caller.emitBidRejected(...)`).
- a property exported as a type the owner plainly contradicts is an error. `int` against
  `real` is not a contradiction, because JavaScript keeps one numeric type and which of
  the two was written was never a promise. `string` against `int` is.

The reading behind all of it is a shape match over QML rather than a compile, so it speaks up
where the owner plainly means something else and stays quiet where it cannot tell. It is
the same reading `synqt infer` prints, and
[`synqt infer --write`](build-system-and-cli.md) fills an empty `export:` in from it.

Mapping to the QtRO semantics the generated rep encodes:

- `prop` generates a property with push semantics. The consumer gets a getter and
  a generated push request; never a direct setter. The owner is the only writer.
  This is the QtRO READPUSH default, and it is deliberate.
- `model` generates a QtRO MODEL exposing only the named roles. Any other field on
  an owner row is invisible to consumers. On the owner, the generated Source
  publishes it two ways, both from the row objects: bind `<model>Rows` to where the
  rows live (for `model items(...)`, `itemsRows: Edge.items`) and every change to them
  republishes, or call `set<Model>(rows)` (`setItems(rows)`) when the rows arrive from
  an event. Either takes an array of row objects as the new authoritative state and
  replicates only the declared roles, so a row may carry extra owner only fields (an
  owner id, a timestamp) that are dropped at the boundary and never serialize to any
  consumer.
  Each role is declared with its type, and a value that will not convert to it
  refuses the publish with a message naming the model, the role and the row, so a
  row that does not match the contract never reaches a consumer at all. Declare a
  role `var` where it genuinely carries anything. Replacing the rows wholesale is
  the owner surface today. Finer grained row updates are an optimization behind the
  same declaration.
- A model travels from the owner to its consumers and no further. A consumer cannot
  write into one, even though the underlying Qt type has a `setData`. A consumer
  that wants owner state changed calls a slot, where `Caller` exists and the owner
  decides.
- `signal` and `slot` map directly: signals run owner to consumer, slots run
  consumer to owner.
- A slot with a return type becomes an asynchronous call on the consumer (a
  pending call that resolves later), because the work happens on the owner. A slot
  with no return type is a one way request. On the consumer it reads as
  `Server.clear().then(ok => ...)`; attach the handler to the call, as there,
  rather than storing the promise and coming back to it in a later frame, because a
  promise is retired once it has settled and delivered.

An `export:` block may also declare plain data records for use in signatures, which
compile to QtRO POD types passed by value:

```yaml
    export: |
      record Address(string[120] street, string[80] city, string[16] zip)
      slot deliver(Address to)
```

### The types a contract can name

The vocabulary is QML's. A value crossing a connect point is read from QML on the owner
side and handed to QML on the consumer side, so the types a contract can name are the
[built-in QML value types](https://doc.qt.io/qt-6/qtqml-typesystem-valuetypes.html) and
nothing invented beside them. What a name means is what the QML documentation says it
means.

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

Four of them can be given a size in brackets, and they are the four with no natural
limit:

| written | bounds |
|---------|--------|
| `string[64]` | at most 64 characters |
| `url[200]` | at most 200 characters |
| `list[100]` | at most 100 elements |
| `var[4096]` | at most 4096 bytes once serialized |

A bound is a rule, not a comment. The owner-side boundary enforces it at every place a
value crosses: an assignment to a bounded `prop`, a role on a published row, an argument
arriving on a `slot`, and an argument leaving on a `signal`. A value that does not fit is
refused and named in a warning, and nothing is truncated: a silently shortened name and a
silently dropped tail are the bugs a bound exists to prevent. Bound the fields that reach
a database column, a filename, or a rendered label, and leave the rest unbounded.

Why a friendlier surface instead of raw rep files. Rep defaults (push versus read
or write, which roles a model exposes) are exactly the places a mistake becomes a
security hole. The `export:` surface keeps the safe defaults obvious and emits
correct rep without the developer memorizing rep keywords. The generated rep is
available in the build directory for inspection.

## Connect points: owned by one entity, consumed by others

An entity has one connect point: the surface it exports, with an owner and a set of
consumers. It has no name of its own, because the owner is its name: consumers reach it as
the owner capitalized, the contract it carries is that plus `Contract`, and the file that
implements it is that plus `.qml`. It is declared in `synqt.yaml` (full schema in
[project layout and configuration](project-layout-and-config.md#the-synqtyaml-schema)):

```yaml
connect_points:
  - owner: edge                   # the entity that holds the authoritative Source
    consumers: [app]              # the entities allowed to acquire the Replica
    server: web/edge/EdgeContract.qml     # the authoritative implementation
    scope: user                   # for browser consumers: minimum session scope
    export: |                     # what may cross it, and nothing else does
      prop int count
```

The configurable parts that matter:

- `owner` and `consumers`. The owner holds the authority. The consumers list is an
  allowlist. Only those entities may acquire the Replica, and the framework opens
  only the mesh links that this implies. A connect point owned by the database and
  consumed by the web edge is reachable by the edge and nobody else, and is never
  reachable by the browser, because the browser is not a listed consumer and
  cannot physically reach the database anyway.
- `scope` (for browser consumers). The minimum session scope a browser user must
  hold before the framework will acquire the Replica for that client. A user below
  the required scope never gets the object, so cannot call its slots at all.
- `export`. What may cross, written on the point. The type it becomes is the owner
  capitalized plus `Contract`, so `owner: edge` exports `EdgeContract`; nothing names it
  separately. The suffix is what keeps it clear of the entity's own singleton, which is the
  owner's name on its own (`web/edge/Edge.qml`): those are two different things, one per
  caller and one for the whole entity, and both are in scope in the same file.
- `server`. The file that implements the connect point, and its root element is the
  contract itself: `web/edge/EdgeContract.qml` opens with `EdgeContract { ... }`. It
  defaults to that name in the owner's folder, so most points never write it. Both ends of
  a contract are QML types with that one name, and they never meet, because an entity may
  not consume the connect point it owns. In an owner's binary `EdgeContract` is the owner
  side; in a consumer's it is the consumer side and the attached handler type used for
  [a connect point's signals](#handling-a-connect-points-signals). Which one you are
  looking at is answered by the file: a Source is the `server:` of a connect point its
  entity owns.

### Gating one member: `<scope>`

`scope:` on the point is all or nothing: below it a visitor acquires no part of the point,
which is what you want when everything it carries is for the same audience. An owner that
serves a public page and an admin surface is not that owner. Write the scope on the member
instead:

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

A member with no gate inherits the point's `scope:`, so the block above reads the way it
looks: moderators get the headline and the catalogue, admins get those and the two gated
members as well. With the default hierarchical scopes a higher scope satisfies a lower one,
which is why admin reaches everything; with `scopes.hierarchical: false` a caller holds
exactly one scope, and a member reachable by two names them both, `<admin,auditor>`.

**The gate is on what crosses, not on what is declared.** The member is still part of the
contract, so a consumer's `Server.storefront` has an `auditLog` model either way. What
changes is that for a caller without the scope it is never seeded, never followed, and
never sent: the rows do not arrive and get hidden, they do not arrive. A gated `slot` is
refused before the owner's QML sees the call, and a gated `signal` is not delivered.

The gate follows the session rather than the connection. A visitor who signs in mid-session
sees what they have just become entitled to without reloading the page, and one whose scope
is taken away has the gated members withdrawn from the replica they are holding.

Two rules `synqt check` enforces, because both are gates that look like protection and are
not: a scope that is not in `scopes.order` can never be held by anyone, and a gate on a
point no client consumes refuses every caller, since a scope belongs to a user's session
and a calling entity has none. Gate a service-to-service member on `Caller.entity` in the
slot instead.

### Handing callers on: `behind:`

Member scopes decide what crosses; `behind:` decides *who answers*. A web edge may own a
point it does not implement and hand each caller to the entity serving people of their
scope:

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
data. The browser writes `Server.gate` whatever answered it. Each entity behind the front
owns an ordinary connect point of its own that the front consumes, and `synqt check` holds
the two together: a tier carries exactly the members the front offers its callers, no more
and no fewer.

The point of the arrangement is what it removes. An entity behind a front is reached by
callers of one scope and no other, so it authorizes on `Caller` and never asks about scope;
nothing enforces that at run time because nothing has to. And with a tier per process, an
admin surface's rows never exist in the process serving anonymous visitors.

A scope with no line of its own is handed to the highest tier at or below what the caller
holds, so `anonymous` and `admin` alone still serve a moderator (the anonymous one). Under
set-based scopes there is no order to fall back along, and a scope nobody named is served by
nobody, which hosts nothing for them.

One thing a front cannot do is answer a slot that returns a value. What it hands the call
to is reached over the mesh and replies after the slot has already returned, so `synqt check`
refuses a returning slot on a fronted point and points at `Caller.emit<Signal>`, which is how
an answer that takes a while gets back to the caller either way.

## How many of an entity there are: `shared`

Read a system as chains. Every chain starts at a browser, which is one person and is never
shared. Next comes the edge it connects to, and after that whatever the edge reaches.
`shared:` is each entity's answer to how many of it there are along that chain:

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

`shared: true` is one Source for the whole entity. Every caller acquires a mirror of it, so
all of them see the same props and the same rows, and each slot still runs with that
caller's `Caller` bound: the mirror is what the caller acquired, so `Caller.hasScope(...)`
still gates, `Caller.entity` still names the calling entity, and `Caller.emit<Signal>`
still reaches that caller and nobody else. It is the natural home for anything everybody
sees: an auction, a leaderboard, the live state of a game.

`shared: false` is one Source per caller. What it holds is that caller's alone. A browser
caller is a session, so their second tab continues what the first tab was using and a
private window gets its own; a mesh caller is the calling entity, so each consuming entity
gets its own. It is what a draft, a wizard's half-filled form or a per-player slice of a
world wants.

It is the entity's answer and not a connect point's, because an entity is one thing
everybody reaches or one thing per caller, and it cannot be both at once for two of its own
surfaces. A point that writes `instance:` is refused by `synqt check`, which names the
entity to write `shared:` on instead.

Two things follow from that:

- **The entity's own `pragma Singleton` file is one either way.** It is the entity itself,
  not a caller's view of it, so it is where state that everybody shares lives when the
  entity is not shared. A not-shared edge with a public feed keeps the feed there and each
  session's Source publishes it.
- **On a shared entity, `Caller` is whoever is calling right now.** Read it in the slot.
  If the work finishes on a later turn, keep what you need in a local first
  (`const who = Caller.session`), because the object itself will have moved on to the next
  caller. On an entity that is not shared there is a Source per caller and its `Caller`
  never changes.

A Source is live state, not storage, whichever answer you give. A per-caller Source lasts
as long as that caller has at least one link open and is gone once they all close, so what
has to survive a user closing the last tab belongs in the singleton or behind a persistence
connect point.

## Reaching a connect point: accessors

How you reach a connect point depends on where your code runs.

From the browser client, the connect points it consumes appear under `Server`,
which is an alias for the web edge this client is attached to:

```qml
// client/TodoView.qml
Label { text: "Items: " + Server.count }          // live property
ListView { model: Server.items }                  // live model
Button { onClicked: Server.add(input.text) }      // a request
EdgeContract.onRejected: reason => banner.show(reason)   // owner explained a refusal
```

From any entity's code, a connect point on another entity appears under that
owner entity's name. For example, inside the web edge's code, the database's
connect points are under `Store`:

```qml
// web/edge/EdgeContract.qml (the edge), calling the store entity
function add(text) {
    if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in first."); return }
    // Persist through the database entity. This is an async cross entity call.
    Store.insert({ text: text.trim(), author: Caller.identity.email })
}
```

`Server` is therefore just the well known name for "the edge a browser client
talks to." The general form is `<EntityName>.<member>`, addressing the owner by
its configured name, capitalized into a QML type like accessor: entity `store`
appears as `Store`, entity `edge` as `Edge`. There is no second name under it,
because an entity has one connect point. (`Server` is the client's alias for its
own edge, whatever that edge entity is named.)

## Handling a connect point's signals

A `Connections` block reacts to a connect point's signals, but it is a lot of
ceremony for the common case:

```qml
Button { onClicked: Server.login(user.text, pass.text) }

Connections {
    target: Server
    function onLoginFailed(reason) { errorPopup.text = reason; errorPopup.open() }
}
```

The contract already declares every signal, so SynQt generates, for each contract,
an attached handler type named after the contract. Write `<Contract>.on<Signal>`
on any element to react to that connect point's signals, with no `target` and no
`function` wrapper, and the handler names are checked against the contract at compile time.
Given an edge that exports `slot login(...)`, `signal loginFailed(string reason)` and
`signal loggedIn()`, the block above becomes two lines:

```qml
Button { onClicked: Server.login(user.text, pass.text) }

EdgeContract.onLoginFailed: reason => { errorPopup.text = reason; errorPopup.open() }
EdgeContract.onLoggedIn:    () => Router.go("/home")
```

The attached type binds to the connect point this entity consumes for that contract, and
there is never more than one, because a contract belongs to an owner, and an owner has one point.

The same shorthand works on the service side, for the signals of a connect point an
entity consumes from another entity. Inside the web edge, reacting to the books entity's
signals, `Connections { target: Books; function onWinnersChanged() {...} }` collapses to:

```qml
BooksContract.onWinnersChanged: hall.refresh()
```

Handlers fire only while the connect point is live; before it is acquired (a browser
below the required scope, or a link still connecting) they do not fire, and
they resume on reconnect, because the framework owns the replica's lifecycle.

The type is named after the contract rather than a single `Self` attached to
everything because a single object could not be checked at compile time (its set of signals would
depend on which owner you meant), and it could not disambiguate a view that reaches two
entities at once. The contract name gives the compiler an exact signal set to verify each
`on<Signal>` against. `Connections` stays available and remains the right tool when the
target is dynamic or is not a connect point.

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
  link the name is trusted by colocation instead, and `Caller.isEntityVerified` is
  false; see [security](security.md).) The owner authorizes by entity: for example a
  store slot can require `Caller.entity === "edge"`.

A calling entity is usually answering somebody, and the framework carries that along:
a connect point a service consumes carries the session the caller is acting for, so
`Caller.identity` and `Caller.hasScope(...)` still mean something on an entity the browser
can never reach. `Caller.isEntity` stays true, because the caller is still that entity;
what it gained is a person behind it, asserted by the entity its certificate identified.
The rules, the limits and what exactly travels are in
[the session down the chain](runtime-api.md#the-session-down-the-chain).

`Client` remains available on web edge connect points as a convenience alias for
`Caller` when the caller is a browser user, so existing edge code reads
directly (`Client.hasScope`, `Client.identity`, `Client.emit<Signal>`, and
`Client.id` for the session id). The general mechanism is `Caller`.

Outside a call that originated from a consumer (an owner-side timer, or the entity's own
singleton) there is no caller, and `Caller` is not in scope there at all. `synqt check`
refuses a file outside a Source that names it, because an authorization line that cannot
run still reads like one to everybody reviewing it.

## A connect point implementation, end to end

`web/edge/EdgeContract.qml`, the authoritative Source on the edge, authorizing the user and
delegating persistence to the database entity:

```qml
import QtQuick
import SynQt

EdgeContract {
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

`db/relational/store/StoreContract.qml`, the authoritative Source on the database entity.
It authorizes nobody, and that is the point: its consumer list has one name in it.

```qml
import QtQuick
import SynQt

StoreContract {
    id: items

    function insert(row) {
        Db.exec("INSERT INTO items(text, author, owner_sub) VALUES(?,?,?)",
                [row.text, row.author, row.ownerSub])   // see docs/entities.md for the Db helper
    }
}
```

Two boundaries, and only one of them is code. The scope on the member decides who may
ask the edge, the edge decides whether to ask the database, and the consumer list decides
who may ask the database at all. `Caller.entity` is what you add to the third when an owner
has two consumers and one of them may do less. Written where there is only one, it repeats
what the topology already proves.

The slot above is worth a test, and the cases worth testing are the ones no UI offers, the
signed out visitor and the oversized item. [Testing your app](testing.md) is how, in QML,
against the real `Caller`.

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
- `Session.state`: `offline`, `connecting`, `connected`, `reconnecting`.
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

A route is compiled into the client bundle with `view:`, or delivered by the web
edge on demand with `remote:`. A `remote:` route names a QML file the edge holds and
sends over the same `wss` link at navigation time, so a peripheral or often-changed
page stays out of the bundle and changes without a client rebuild. Unlike a compiled
in view, a delivered page's `scope` is enforced on the edge before delivery, so its
markup never reaches an under scoped machine. The data it later reads is still
governed by the connect point's own scope, as always. See
[remote pages](remote-pages.md).

## Connection lifecycle and offline behavior

Each link uses a QtRO heartbeat so a dropped connection is noticed promptly rather
than only on the next send (QtRO disables the heartbeat by default, and SynQt enables
it). On a browser disconnect, `Session.state` becomes `reconnecting` and the
client retries with capped exponential backoff. Replicas report not ready and QML
can show cached values or an offline banner. A session the edge rejects (expired or
revoked credential) is the same state, because the browser does not say why a
handshake failed. What an app watches for there is `Session.isAuthenticated` going
false and its scope-gated replicas being released.
Service to service links reconnect the same way. An entity that loses a consumed
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
