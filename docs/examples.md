# Examples

These are complete, worked applications expressed in the SynQt programming model.
They show every artifact a developer writes: contracts, configuration, server
implementations, and client views. They do not show framework internals. Each
example builds on the previous one.

Examples 1 to 3 are single edge systems (a client and a web edge): the edge owns
its state in memory. They use `Client` to reach the calling browser session, which
is the web edge alias for the general `Caller` accessor described in
[the programming model](programming-model.md#reaching-the-caller-the-caller-accessor).
Example 4 adds a third entity, a database, and shows the mesh: the edge authorizes
the user, then calls the database, which authorizes the edge. Example 5 keeps that
three-entity shape and adds [remote pages](remote-pages.md): views the web edge
delivers on demand rather than compiling into the client bundle.

## Example 1: a shared live counter (no login)

The smallest non trivial app: a counter every connected client sees update in
real time. It demonstrates a `shared` connect point, an edge owned property, and
a client to edge request.

### Contract, `shared/Counter.syn`

```syn
contract Counter {
    prop int value          // edge owned; clients read, edge writes
    slot increment()        // a request; the edge performs the change
    slot decrement()
}
```

### Configuration, `synqt.yaml`

```yaml
project:
  name: counter
  version: 0.1.0
  qt_version: 6.11.1

entities:
  - name: client
    kind: client
    path: client
    entry: client/Main.qml
    edge: web

  - name: web
    kind: service
    path: web
    capabilities: [web_edge]
    public:
      port: 8443

connect_points:
  - name: counter
    contract: Counter
    owner: web                # the edge holds the authoritative Source
    consumers: [client]       # the browser may acquire it
    server: web/Counter.qml
    instance: shared          # one counter, shared by all clients
    # no scope: any session may use it
```

No `identity` section, so every connection runs at the default anonymous scope.

This config is the development shape: `synqt dev` serves it plaintext on
localhost. A release build refuses to start without TLS, so running it with
`synqt serve` additionally needs a `tls` section on the web entity with a
certificate, as Example 2 shows (see the
[validation rules](project-layout-and-config.md#validation)).

### Edge, `web/Counter.qml`

```qml
import QtQuick
import SynQt

CounterSource {
    id: counter
    value: 0

    function increment() { counter.value = counter.value + 1 }   // the edge is the writer
    function decrement() { counter.value = counter.value - 1 }
}
```

Because `value` is a contract property, the framework pushes every change to all
replicas. No broadcast code is needed.

### Client, `client/Main.qml`

```qml
import QtQuick
import QtQuick.Controls
import SynQt

ApplicationWindow {
    visible: true
    title: "Counter"

    Column {
        anchors.centerIn: parent
        spacing: 12

        Label {
            text: Session.state === "connected" ? ("Value: " + Server.counter.value)
                                                : "Connecting..."
            font.pixelSize: 28
        }

        Row {
            spacing: 8
            Button { text: "-"; onClicked: Server.counter.decrement() }
            Button { text: "+"; onClicked: Server.counter.increment() }
        }
    }
}
```

Open the page in two browser tabs and the counter stays in sync, because both
replicas observe the same shared Source.

## Example 2: the authenticated Todo app

This is the canonical SynQt example: a shared todo list where anyone may read,
but only signed in users may add, and a user may only remove their own items.
Moderators may remove anything. It demonstrates login, scopes, per row ownership
that never leaves the edge, and an edge to client refusal channel.

### Contract, `shared/Todo.syn`

```syn
contract Todo {
    prop int count                          // number of items, edge owned
    model items(text, author, done)         // only these roles cross to clients
    slot add(string text)
    slot remove(int index)
    signal rejected(string reason)          // the edge explains a refusal to one client
}
```

Note what is absent from the model role list: there is no `ownerId`. The edge
will keep an owner id per row for authorization, and it will never reach any
client because it is not a declared role.

### Configuration, `synqt.yaml`

```yaml
project:
  name: todo
  version: 0.1.0
  qt_version: 6.11.1

scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous

entities:
  - name: client
    kind: client
    path: client
    entry: client/Main.qml
    edge: web

  - name: web
    kind: service
    path: web
    capabilities: [web_edge]
    public:
      port: 8443
    tls:
      cert_file: certs/fullchain.pem
      key_file: certs/privkey.pem
    env:
      file: web/.env

identity:
  required: false                 # anonymous users may read; only writing needs a scope
  login: /auth/login
  callback: /auth/callback
  providers:
    - name: github
      authorize_url: https://github.com/login/oauth/authorize
      token_url: https://github.com/login/oauth/access_token
      userinfo_url: https://api.github.com/user
      client_id: your-github-client-id
      client_secret: env:GITHUB_CLIENT_SECRET
      scopes: [read:user, user:email]
  mapping:
    hook: web/identity/map.qml

connect_points:
  - name: todo
    contract: Todo
    owner: web
    consumers: [client]
    server: web/Todo.qml
    instance: shared              # one list everyone sees
    # no scope on the connect point: anonymous users may acquire it and read.
    # write permission is enforced inside the slots, not at acquisition.
```

`web/.env` (edge only, never shipped):

```cli
GITHUB_CLIENT_SECRET=the-real-secret-value
```

### Identity mapping, `web/identity/map.qml`

This optional hook turns a provider identity into a SynQt scope after login. It
runs only on the edge.

```qml
import QtQuick
import SynQt

IdentityMapping {
    // Return the scope a freshly authenticated identity should hold.
    function scopeFor(identity) {
        const admins      = ["hello@iamki.dev"]
        const moderators  = ["mod@example.com"]
        if (admins.indexOf(identity.email) !== -1)     return "admin"
        if (moderators.indexOf(identity.email) !== -1) return "moderator"
        return "user"   // any successfully authenticated user
    }
}
```

### Edge, `web/Todo.qml`

```qml
import QtQuick
import SynQt

TodoSource {
    id: todo
    count: 0

    // Authoritative rows. ownerId is edge only and never crosses to clients.
    property var rows: []

    function add(text) {
        if (!Client.hasScope("user")) {
            Client.emitRejected("Sign in to add items.")
            return
        }
        const clean = ("" + text).trim()
        if (clean.length === 0 || clean.length > 280) {
            Client.emitRejected("Items must be 1 to 280 characters.")
            return
        }
        rows.push({
            text: clean,
            author: Client.identity.email,
            done: false,
            ownerId: Client.id              // edge only authorization data
        })
        todo.setItems(rows)                 // replicate; only declared roles cross
        todo.count = rows.length
    }

    function remove(index) {
        if (index < 0 || index >= rows.length) {
            Client.emitRejected("No such item.")
            return
        }
        const row = rows[index]
        const isOwner = row.ownerId === Client.id
        if (!isOwner && !Client.hasScope("moderator")) {
            Client.emitRejected("You can only remove your own items.")
            return
        }
        rows.splice(index, 1)
        todo.setItems(rows)
        todo.count = rows.length
    }
}
```

### Client, `client/Main.qml`

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

ApplicationWindow {
    visible: true
    width: 360; height: 480
    title: "Todo"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Label { text: "Todo"; Layout.fillWidth: true }
            Button {
                text: Session.identity ? Session.identity.email : "Sign in"
                onClicked: if (!Session.identity) Session.login()   // sends browser to /auth/login
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        Label { text: "Items: " + (Server.todo.count || 0) }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: Server.todo.items
            delegate: RowLayout {
                width: ListView.view.width
                CheckBox { checked: model.done; enabled: false }
                Label { text: model.text + "  (" + model.author + ")"; Layout.fillWidth: true }
                Button {
                    text: "Remove"
                    // UX hint only; the edge enforces ownership regardless.
                    visible: Session.hasScope("user")
                    onClicked: Server.todo.remove(index)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: "New item"
                enabled: Session.hasScope("user")
            }
            Button {
                text: "Add"
                enabled: Session.hasScope("user") && input.text.trim().length > 0
                onClicked: { Server.todo.add(input.text); input.text = "" }
            }
        }
    }

    // The edge's refusal channel: show why an action was rejected.
    Todo.onRejected: reason => { toast.text = reason; toast.open() }

    Popup { id: toast; property alias text: msg.text; Label { id: msg } }
}
```

### What this example demonstrates

- The client treats `add` and `remove` as requests. The edge authorizes them.
- `ownerId` exists on every row for authorization and never crosses the boundary,
  because it is not a declared model role. Confidentiality is structural.
- Client side `Session.hasScope("user")` only hides and disables UI. A modified
  client that calls `Server.todo.add` while anonymous still hits an edge that
  refuses, with `rejected("Sign in to add items.")`.
- Ownership is enforced with an edge side value the client cannot spoof, never with
  anything the client sends. Here that value is `Client.id`, the session identifier;
  `Client` is the edge alias for the `Caller` accessor, so this is the same mechanism
  the mesh example (Example 4) writes as `Caller`. A session id is enough for this in
  memory list, where the data lives only as long as the edge process. Durable rows
  key ownership on `Caller.identity.sub` instead (see Example 4), so ownership
  survives a new session and a restart.
- The whole login exchange (the GitHub redirect, the code exchange with the
  client secret, the session cookie) happens on the edge. The browser only ever
  holds the opaque session cookie.

## Example 3: a private per session draft (sketch)

When state must be private to one client, mark the connect point `per_session` so
each session gets its own authoritative Source. The contract and client code look
the same as a shared connect point; only the configuration differs:

```yaml
connect_points:
  - name: draft
    contract: Draft
    owner: web
    consumers: [client]
    server: web/Draft.qml
    scope: user               # only signed in users may acquire it at all
    instance: per_session     # each session has its own draft Source
```

Now one user's draft is a different Source instance from another's. There is no
shared object through which one client could observe another's draft, and the
`scope: user` precondition means an anonymous client never even acquires the
replica.

## Example 4: a three entity todo with durable storage

This is the canonical mesh example. A browser client, a web edge, and a database
entity. The edge owns the user facing connect point and authorizes the user; the
database owns durable storage and authorizes the edge. Items survive a restart
because they live in the database entity, not in edge memory.

### Topology, `synqt.yaml`

```yaml
project:
  name: todo
  version: 0.1.0
  qt_version: 6.11.1
  origin_model: same_origin

scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous

entities:
  - name: client
    kind: client
    path: client
    entry: client/Main.qml
    edge: web

  - name: web
    kind: service
    path: web
    capabilities: [web_edge]
    public:
      host: 0.0.0.0
      port: 8443
    tls:
      cert_file: certs/web/fullchain.pem
      key_file: certs/web/privkey.pem
    mesh:
      transport: mtls            # the default: mutual TLS, over loopback on one host
      host: 127.0.0.1
      port: 9443
    env:
      file: web/.env

  - name: database
    kind: service
    path: database
    blueprint: persistence
    mesh:
      transport: mtls            # certificate identity: the database can trust Caller.entity
      host: 127.0.0.1
      port: 9444
    settings:
      file: database/data/app.db
      journal_mode: wal
      busy_timeout_ms: 5000

connect_points:
  - name: todo
    contract: Todo
    owner: web                  # the edge owns the user facing object
    consumers: [client]         # the browser may acquire it
    server: web/Todo.qml
    instance: shared

  - name: items
    contract: Items
    owner: database             # the database owns durable storage
    consumers: [web]            # only the edge may reach it; never the browser
    server: database/Items.qml
    instance: shared
```

Auth is added with `synqt add auth github` (see [authentication](authentication.md)); omitted here for focus.

The mesh link is mutual TLS even though both entities share a host, so the
database's `Caller.entity` check below rests on a verified certificate.
`synqt mesh cert --all` issues the certificates for deployment; `synqt dev`
provisions throwaway development ones automatically.

### Contracts, `shared/`

`shared/Todo.syn` (browser facing, owned by the edge):

```syn
contract Todo {
    model items(text, author, done)   // only these roles cross to the browser
    slot add(string text)
    slot remove(int index)
    signal rejected(string reason)
}
```

`shared/Items.syn` (internal, owned by the database, consumed by the edge):

```syn
contract Items {
    slot var list()                    // rows { id, text, author, ownerSub } to the edge
    slot insert(ItemRow row)
    slot remove(int id)
    signal changed()                   // tells the edge the data moved
}

record ItemRow(string text, string author, string ownerSub)
```

Note `ownerSub` exists on the internal contract (the edge needs it to enforce
ownership) but is absent from `Todo.items` roles, so it never reaches the browser.

### The database entity, `database/Items.qml`

```qml
import QtQuick
import SynQt

ItemsSource {
    id: items

    function list() {
        // Only the edge may read. Authorize the calling entity.
        if (Caller.entity !== "web") return []
        return Db.query("SELECT id, text, author, owner_sub AS ownerSub FROM items ORDER BY id DESC LIMIT 200")
    }

    function insert(row) {
        if (Caller.entity !== "web") return
        Db.exec("INSERT INTO items(text, author, owner_sub) VALUES(?, ?, ?)",
                [row.text, row.author, row.ownerSub])   // parameterized: no injection
        items.changed()                                  // notify the edge
    }

    function remove(id) {
        if (Caller.entity !== "web") return
        Db.exec("DELETE FROM items WHERE id = ?", [id])
        items.changed()
    }
}
```

`database/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS items (
    id        INTEGER PRIMARY KEY,
    text      TEXT NOT NULL,
    author    TEXT NOT NULL,
    owner_sub TEXT NOT NULL
);
```

### The web edge, `web/Todo.qml`

```qml
import QtQuick
import SynQt

TodoSource {
    id: todo

    // The last fetched internal rows (id and ownerSub included): edge memory only,
    // used to authorize removals. Never a model role, so it never reaches a browser.
    property var rows: []

    // Keep the browser facing model in sync with the database.
    function refresh() {
        // list() returns a value, so this cross entity call resolves asynchronously.
        Database.items.list().then(fetched => {
            todo.rows = fetched
            // Map internal rows to the browser facing roles (drop id and ownerSub).
            todo.setItems(fetched.map(r => ({ text: r.text, author: r.author, done: false })))
        })
    }

    Component.onCompleted: refresh()

    Items.onChanged: todo.refresh()   // the database moved; repull

    function add(text) {
        // The edge authorizes the user.
        if (!Caller.hasScope("user")) { Caller.emitRejected("Sign in to add items."); return }
        const clean = ("" + text).trim()
        if (clean.length === 0 || clean.length > 280) {
            Caller.emitRejected("Items must be 1 to 280 characters."); return
        }
        // Persist via the database entity. The database authorizes that the caller is the edge.
        Database.items.insert({ text: clean, author: Caller.identity.email, ownerSub: Caller.identity.sub })
    }

    function remove(index) {
        if (index < 0 || index >= rows.length) {
            Caller.emitRejected("No such item."); return
        }
        // Ownership is decided against the verified identity, never a client value:
        // a user removes only rows whose ownerSub matches their own sub; a moderator
        // removes any.
        const row = rows[index]
        const isOwner = Caller.identity && row.ownerSub === Caller.identity.sub
        if (!isOwner && !Caller.hasScope("moderator")) {
            Caller.emitRejected("You can only remove your own items."); return
        }
        // The database authorizes that the caller is the edge, then deletes by id.
        Database.items.remove(row.id)
    }
}
```

### The client, `client/Main.qml`

Identical in spirit to Example 2: it reads `Server.todo.items`, calls
`Server.todo.add(...)` and `Server.todo.remove(index)`, and shows
`Server.todo.rejected` reasons. The client does
not know a database exists; it only ever talks to the edge.

### What this example demonstrates

- Three entities, two trust boundaries. The edge authorizes the user
  (`Caller.hasScope`), the database authorizes the entity (`Caller.entity`). Neither
  trusts the other blindly.
- The full user authorization matrix lives on the edge: anonymous cannot add, a
  user removes only rows whose `ownerSub` matches their own `Caller.identity.sub`,
  a moderator removes any. No client supplied value participates in the ownership
  decision; the edge compares its own cached `ownerSub` against the verified
  identity.
- The browser cannot reach the database. `items` lists only `web` as a consumer, and
  the browser cannot physically reach a non edge entity anyway.
- Data minimization across two hops. `ownerSub` is on the internal contract for the
  edge's ownership logic and is dropped before anything reaches the browser, because
  it is not a `Todo.items` role. It carries `Caller.identity.sub`, the stable
  identity subject, rather than the session id (`Client.id`) that Example 2 used:
  the accessor is the same one Example 2 reaches through the `Client` alias, but a
  durable row must stay owned across new sessions and restarts, so it keys on the
  identity rather than the session.
- Durability without a third party database server. Items live in the persistence
  entity's embedded store and survive restarts. No separate database product is run,
  configured, or secured; it is a SynQt entity in the same toolchain and security
  model.
- The same connect point mechanism carries both links. `Server.todo` (browser to
  edge over wss) and `Database.items` (edge to database over the mesh) are the same
  programming model with different transports underneath.

## Example 5: a storefront with edge-delivered campaign pages

The [`stall`](https://github.com/Kidev/SynQt/tree/main/examples/stall) example is the
three-entity shape of Example 4 (a browser client, a web edge, and a `stock` database
the browser reaches only through the edge) with one thing added: its marketing
campaign pages are [remote pages](remote-pages.md), delivered by the edge on demand
rather than compiled into the client bundle. The product grid and the cart ship in the
bundle; a merchandiser changes a campaign, or adds a new one, without a client rebuild.

### Topology, `synqt.yaml`

The entities are a `kind: client`, a `capability: web_edge`, and a
`blueprint: persistence` database. The route table and the `router` block are
top-level keys:

```yaml
routes:
  - path: /
    view: Home.qml            # compiled into the client bundle
  - path: /cart
    view: Cart.qml

  - path: /c/:campaign
    remote: Campaign.qml      # delivered by the edge, from web/pages/Campaign.qml
    seed: web/campaign-seed.qml
  - path: /members
    remote: Members.qml       # delivered by the edge, and members only
    scope: user

router:
  fallback: /
  base: /
  palette: [QtQuick, QtQuick.Layouts]   # what a delivered page may import

connect_points:
  - name: catalog
    contract: Catalog
    owner: web                # the edge owns the browser-facing live catalog
    consumers: [client]
    server: web/Catalog.qml
    instance: shared

  - name: inventory
    contract: Inventory
    owner: stock              # the database owns the durable stock
    consumers: [web]          # only the edge; a client consumer here fails synqt check
    server: stock/Inventory.qml
    instance: per_peer
```

### The delivered page, `web/pages/Campaign.qml`

One file serves every slug. Its root is an `Item`, not a window, because a delivered
page is loaded into the client's `Loader`, and it imports only the palette modules. It
paints its headline from the seed on the first frame, then keeps the offers live
through the `catalog` replica:

```qml
import QtQuick
import QtQuick.Layouts

Item {
    id: campaign

    readonly property string headline: Router.pageSeed.headline ?? qsTr("Today's offers")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            text: campaign.headline
            font.pixelSize: 24
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Server.Catalog.offers
            delegate: Text {
                required property string title
                required property int price
                width: ListView.view.width
                text: qsTr("%1  -  %2").arg(title).arg(price)
            }
        }
    }
}
```

### The page seed, `web/campaign-seed.qml`

The seed runs on the edge, after the route's scope check, and turns the slug into the
headline the page paints first, so it never flashes empty:

```qml
import QtQuick
import SynQt

PageSeed {
    // Leave the parameters untyped: the edge invokes this hook generically, passing
    // every argument as a QVariant, so annotating one (route: string) would change the
    // method signature and the edge would silently deliver the page with no seed. The
    // return may be annotated var.
    function seedFor(route, parameters, caller): var {
        const slug = parameters.campaign ?? "";
        const words = slug.split("-").filter(part => part.length > 0);
        const headline = words
            .map(part => part.charAt(0).toUpperCase() + part.slice(1))
            .join(" ");
        return { headline: headline.length > 0 ? headline : qsTr("Today's offers") };
    }
}
```

### What this example demonstrates

- A route is compiled in (`view:`) or edge-delivered (`remote:`). The campaign and
  members pages are `remote:`, so they never enter the bundle and change without a
  client rebuild.
- `router.palette` is the trust boundary for a delivered page: the whole set of QML
  modules it may import, enforced by the client.
- The page seed paints the first frame. It runs on the edge per request, is keyed on
  the path parameter, and its return is `Router.pageSeed` on the client, so one
  `Campaign.qml` gives every slug its own headline. Its parameters are left untyped so
  the edge's generic invocation matches.
- A delivered page's `scope` protects the page, not the data. `Members.qml` is refused
  to an under-scoped session with no markup, no hash, and no seed, but the data any
  page reads is still governed by the connect point's own scope.
- The database stays unreachable from the browser. The `inventory` connect point is
  owned by `stock` and consumed only by `web`; adding the client as a consumer fails
  `synqt check`, because the browser can only reach a web edge.

The two [remote-pages tutorials](tutorial-remote-pages.md) build this storefront up
step by step and run the three hands-on checks against it.
