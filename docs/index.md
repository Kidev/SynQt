---
hide:
  - navigation
  - toc
---

<div class="synqt-home" markdown>

<div class="synqt-hero" markdown>

<div class="synqt-hero__inner" markdown>

<div class="synqt-hero__text" markdown>

<p class="synqt-eyebrow">QML / one toolchain / zero third party servers</p>

<div class="synqt-headline" markdown>
Build complete web systems with QML, with no third party servers to stand up.
</div>

SynQt (pronounced synced) is built from entities: a browser client, a web
edge, a database, and whatever else your system needs, each its own binary,
sharing one toolchain and one security model.

<div class="synqt-hero__actions synqt-actions">
<a class="cta" href="quick-start/" markdown="0"><span class="span">Get started</span><span class="second"><svg width="50px" height="20px" viewBox="0 0 66 43" version="1.1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"><g id="arrow" stroke="none" stroke-width="1" fill="none" fill-rule="evenodd"><path class="one" d="M40.1543933,3.89485454 L43.9763149,0.139296592 C44.1708311,-0.0518420739 44.4826329,-0.0518571125 44.6771675,0.139262789 L65.6916134,20.7848311 C66.0855801,21.1718824 66.0911863,21.8050225 65.704135,22.1989893 C65.7000188,22.2031791 65.6958657,22.2073326 65.6916762,22.2114492 L44.677098,42.8607841 C44.4825957,43.0519059 44.1708242,43.0519358 43.9762853,42.8608513 L40.1545186,39.1069479 C39.9575152,38.9134427 39.9546793,38.5968729 40.1481845,38.3998695 C40.1502893,38.3977268 40.1524132,38.395603 40.1545562,38.3934985 L56.9937789,21.8567812 C57.1908028,21.6632968 57.193672,21.3467273 57.0001876,21.1497035 C56.9980647,21.1475418 56.9959223,21.1453995 56.9937605,21.1432767 L40.1545208,4.60825197 C39.9574869,4.41477773 39.9546013,4.09820839 40.1480756,3.90117456 C40.1501626,3.89904911 40.1522686,3.89694235 40.1543933,3.89485454 Z" fill="#FFFFFF"></path><path class="two" d="M20.1543933,3.89485454 L23.9763149,0.139296592 C24.1708311,-0.0518420739 24.4826329,-0.0518571125 24.6771675,0.139262789 L45.6916134,20.7848311 C46.0855801,21.1718824 46.0911863,21.8050225 45.704135,22.1989893 C45.7000188,22.2031791 45.6958657,22.2073326 45.6916762,22.2114492 L24.677098,42.8607841 C24.4825957,43.0519059 24.1708242,43.0519358 23.9762853,42.8608513 L20.1545186,39.1069479 C19.9575152,38.9134427 19.9546793,38.5968729 20.1481845,38.3998695 C20.1502893,38.3977268 20.1524132,38.395603 20.1545562,38.3934985 L36.9937789,21.8567812 C37.1908028,21.6632968 37.193672,21.3467273 37.0001876,21.1497035 C36.9980647,21.1475418 36.9959223,21.1453995 36.9937605,21.1432767 L20.1545208,4.60825197 C19.9574869,4.41477773 19.9546013,4.09820839 20.1480756,3.90117456 C20.1501626,3.89904911 20.1522686,3.89694235 20.1543933,3.89485454 Z" fill="#FFFFFF"></path><path class="three" d="M0.154393339,3.89485454 L3.97631488,0.139296592 C4.17083111,-0.0518420739 4.48263286,-0.0518571125 4.67716753,0.139262789 L25.6916134,20.7848311 C26.0855801,21.1718824 26.0911863,21.8050225 25.704135,22.1989893 C25.7000188,22.2031791 25.6958657,22.2073326 25.6916762,22.2114492 L4.67709797,42.8607841 C4.48259567,43.0519059 4.17082418,43.0519358 3.97628526,42.8608513 L0.154518591,39.1069479 C-0.0424848215,38.9134427 -0.0453206733,38.5968729 0.148184538,38.3998695 C0.150289256,38.3977268 0.152413239,38.395603 0.154556228,38.3934985 L16.9937789,21.8567812 C17.1908028,21.6632968 17.193672,21.3467273 17.0001876,21.1497035 C16.9980647,21.1475418 16.9959223,21.1453995 16.9937605,21.1432767 L0.15452076,4.60825197 C-0.0425130651,4.41477773 -0.0453986756,4.09820839 0.148075568,3.90117456 C0.150162624,3.89904911 0.152268631,3.89694235 0.154393339,3.89485454 Z" fill="#FFFFFF"></path></g></svg></span></a>
<a class="cta cta--quiet" href="#what-it-looks-like" markdown="0"><span class="span">What it looks like</span></a>
</div>

<div class="synqt-hero__install" markdown>

<p class="synqt-hero__install-icons" markdown="span">:material-apple:{ .synqt-hero__install-icon }:material-linux:{ .synqt-hero__install-icon }</p>

```cli
curl -fsSL https://get.synqt.org/install.sh | sh
```

</div>

<div class="synqt-hero__install synqt-hero__install--stacked" markdown>

<p class="synqt-hero__install-icons" markdown="span">:material-microsoft-windows:{ .synqt-hero__install-icon }</p>

```cli
irm https://get.synqt.org/install.ps1 | iex
```

</div>

<div class="synqt-hero__install synqt-hero__install--stacked" markdown>

<p class="synqt-hero__install-icons" markdown="span">:material-language-python:{ .synqt-hero__install-icon }</p>

```cli
pipx install synqt
```

</div>

</div>

<img src="assets/synqt.svg" alt="SynQt" class="synqt-hero__mark">

</div>

</div>

<div class="synqt-section" markdown>

## Why SynQt

<div class="grid cards" markdown>

-   :material-language-markdown: __One language, front to back__

    Write the UI and the server side logic in QML. The boundary between any two
    components is a set of typed connect points, named and access controlled by
    configuration.

-   :material-flash: __Live by default__

    A value that updates across every browser the instant it changes, with no
    manual wiring and no client side polling, is a few lines of QML.

-   :material-database: __Batteries included, no third party servers__

    Add a database, cache, document store, gateway, or jobs runner as a first party
    entity. Back it with an embedded engine, or mask PostgreSQL, MongoDB, or Redis
    behind it with one config value.

-   :material-devices: __Web and desktop, one codebase__

    The client is a Qt app. Ship it to the browser as WebAssembly and, from the same
    QML, as a native app for Windows, macOS, and Linux, against the same edge and the
    same security model.

-   :material-shield-lock: __Secure at every link__

    Every link is encrypted and authenticated from the first build. There is no
    plaintext connection type to reach for by mistake, only the one every entity
    already speaks.

-   :material-radar: __One click, one trace__

    Add a monitor and every entity reports to it, so a click in the browser becomes one
    trace running through each entity it touched. Nothing is recorded until you add one,
    and turning a category up during an incident is a restart, not a rebuild.

</div>

</div>

<div class="synqt-section" markdown>

## A closer look

<div class="synqt-deepdive" markdown>

<div class="synqt-deepdive__item" markdown>

### Contracts and connect points

Two entities talk through a connect point, a named, typed, live object that one
entity owns and the others see a live copy of. Properties and signals flow from the
owner to every consumer. Slots flow the other way, and the owner always decides.

[Read the programming model&nbsp;&rarr;](programming-model.md)

</div>

<div class="synqt-deepdive__item" markdown>

### Security by default

TLS everywhere, mutual TLS between entities, a deny by default topology, and data
minimization built into the contract format itself. There is no insecure middle
state a project can accidentally ship in.

[Read the security model&nbsp;&rarr;](security.md)

</div>

<div class="synqt-deepdive__item" markdown>

### One toolchain

The `synqt` CLI pins the exact Qt and Emscripten versions your project needs and
prints the commands that install them, builds every entity, native and WebAssembly
alike, and runs them all together with file watching and hot reload.

[Read the build system and CLI guide&nbsp;&rarr;](build-system-and-cli.md)

</div>

</div>

</div>

<div class="synqt-section" markdown>

## What it looks like

A chat room, because everyone already knows what one does. Somebody types a line and it
appears in every window that has the room open, including the ones on other machines.
That is the part SynQt is for, and below is all of it.

A finished system is a small mesh of entities. The drawing below is that mesh, drawn by the
design editor from this very project: three boxes saying which side of the wire each entity
is on, a shape per entity, and on every line the contract the two ends share. Only the web
edge faces the internet; everything else sits in the mesh box and is reachable only by the
entities you allow.

A visitor who has signed in as nobody gets a sign-in page, and the room is not hidden from
them so much as absent: the point that carries it is gated `scope: user`, so their session
never acquires it and there is nothing on their side to get past. Signing in fills the same
window with the room. A moderator gets one member more than everybody else, `erase`, and
gets it because the contract says so and not because the client decided to offer it.

Five files are the whole system: one configuration file, which says what crosses each link,
one QML file per entity, and the table the database keeps the messages in. Hover (or focus)
an entity to read the file it is, or the mark on a line to read the block that says what
crosses it, and the card that opens says the rest. The project tree under the drawing opens
the same files, and one stays open until you move to another. The database opens two, its
QML and the table that QML queries, since neither says much without the other. The files
carry no explanatory comments: a line with something to say about itself is marked down its
left instead, and hovering it says the thing. A line that ends in an arrow opens the page
covering it, whether that is a page of this guide or the class in the C++ reference.

The button under it opens this same drawing in the
[online designer](visual-editor.md), which runs in the browser with nothing installed. It is
the same code that drew it here, so nothing is lost on the way: pull the mesh apart there,
add an entity, and export the result as a project.

<div class="synqt-explorer">

<div class="synqt-mesh">

<div class="synqt-flow">
<!-- Drawn by the design editor's own code, from the same project the editor opens at
     /designer/#example=demo: docs/javascripts/home-flow.js imports /designer/canvas.js,
     reads the document out of /designer/examples.json and hands the two to `draw`. A
     second, hand-kept picture of the same system would drift the first time the demo
     changed.

     `data-files` is the one thing this page adds: which file each part of the drawing
     opens. It is written here rather than in the script because it names the files below,
     and the drawing's own names (an entity, a connect point) are what it keys on. -->
<div class="synqt-flow__stage" id="synqt-flow-stage" data-example="demo"
     data-files='{"entity:app": "client", "entity:edge": "edge",
                  "entity:store": "database schema",
                  "contract:edge": "config", "contract:store": "config"}'>
</div>
</div>

</div>

<div class="synqt-explorer__view">
<div class="synqt-tree">
<span class="synqt-tree__title">Project tree</span>
<ul class="synqt-tree__list">
<li class="synqt-tree__leaf"><span class="synqt-tree__file" data-file="config" tabindex="0" role="button" aria-label="Show synqt.yaml">synqt.yaml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="client" tabindex="0" role="button" aria-label="Show the client entity">client</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">app</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">Main.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="edge" tabindex="0" role="button" aria-label="Show the web edge entity">web</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="edge" tabindex="0" role="button" aria-label="Show web/edge/Edge.qml">edge</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="edge" tabindex="0" role="button" aria-label="Show web/edge/Edge.qml">Edge.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="database schema" tabindex="0" role="button" aria-label="Show the relational entity">db/relational</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="database schema" tabindex="0" role="button" aria-label="Show the store entity's files">store</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="database schema" tabindex="0" role="button" aria-label="Show db/relational/store/Store.qml and db/relational/store/schema.sql">Store.qml</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="database schema" tabindex="0" role="button" aria-label="Show db/relational/store/Store.qml and db/relational/store/schema.sql">schema.sql</span></li>
</ul>
</div>


<div class="synqt-explorer__files">

<div class="synqt-file" data-file="config" markdown>
<span class="synqt-file__name"><strong>configuration</strong><span class="synqt-flow__path">synqt.yaml</span></span>

```yaml
project:
  name: chat
  qt_version: 6.11.1

scopes: { order: [anonymous, user, admin], default: anonymous }

identity:
  providers: [{ name: github, client_id: ..., client_secret: env:SECRET }]
  mapping: web/edge/identity/map.qml

entities:
  - { name: app, type: client }
  - name: edge
    type: web_edge
    identity: true
    public: { port: 8443, sync_route: /sync }
  - { name: store, type: relational, provider: { name: sqlite } }

connect_points:
  - owner: edge
    consumers: [app]
    scope: user
    export: |
      model messages(int id, string[40] who, string[280] body, bool staff)
      slot say(string[280] body)
      <admin> slot erase(int id)
  - owner: store
    consumers: [edge]
    export: |
      prop var[24000] lines
      slot say(string[40] who, string[280] body, bool staff)
      slot erase(int id)
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="qt_version" data-href="build-system-and-cli/">One version pins the whole toolchain: Qt, the Emscripten it is built against, and every entity built from them.</li>
<li data-code="order: [anonymous" data-href="security/">The scope ladder. Every session sits on one rung, and a connect point can demand a minimum.</li>
<li data-code="mapping: web/edge/identity/map.qml" data-href="authentication/">The one place that decides who is a moderator. It turns a verified login into a scope, and everything else in the system reads that answer rather than making it.</li>
<li data-code="name: app, type: client" data-href="desktop/">The room, built to WebAssembly. The same QML also builds as a native app for Windows, macOS, and Linux, against this same edge.</li>
<li data-code="type: web_edge" data-href="entities/">The one entity allowed to face the internet, on the one public port. Nothing else gets one.</li>
<li data-code="identity: true" data-href="authentication/">This edge runs the OAuth exchange and keeps the sessions, which is what gives `Session.login()` in the client something to reach.</li>
<li data-code="type: relational" data-href="providers/">A database entity. It embeds SQLite by default, and one config value puts PostgreSQL or MySQL behind the same interface.</li>
<li data-code="scope: user" data-href="programming-model/">The gate on the whole point. A session that has not signed in never acquires it, so there is no room on their side to get past.</li>
<li data-code="consumers: [app]" data-href="project-layout-and-config/">The browser's one way in. An entity that is not on this list cannot open this connect point at all.</li>
<li data-code="&lt;admin&gt; slot erase(int id)" data-href="security/">The gate is on the member. A caller without the scope does not have the slot, so the edge refuses the call before it runs, and there is no check to write, or forget, in the QML behind it.</li>
<li data-code="model messages(int id" data-href="programming-model/">The roles listed here are the whole of what a message may carry to a browser. `said_at` is in the table and not in this line, so it never leaves the mesh.</li>
<li data-code="consumers: [edge]" data-href="entities/">The database is reachable by the edge and by nothing else. The browser is on no list here, so there is no request it can make.</li>
<li data-code="prop var[24000] lines" data-href="programming-model/">The room, held in one place and mirrored by the edge. The bracketed number is the limit the owner holds it to at the boundary, in bytes on the wire.</li>
</ul>

</div>

<div class="synqt-file" data-file="client" markdown>
<span class="synqt-file__name"><strong>app</strong><span class="synqt-flow__path">client/app/Main.qml</span></span>

```qml
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    visible: true
    title: qsTr("The chat room")

    ColumnLayout {
        anchors.centerIn: parent
        visible: !Session.hasScope("user")
        spacing: 24

        Label {
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 32
            text: qsTr("One room. Everybody in it sees the same thing.")
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Sign in with GitHub")
            onClicked: Session.login()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: Session.hasScope("user")

        ListView {
            id: messages

            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            model: Server.messages

            delegate: Item {
                id: line

                required property var model

                width: messages.width
                height: 26

                Label {
                    x: 8
                    width: 132
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: line.model.staff ? "#d0342c" : window.palette.windowText
                    font.bold: line.model.staff
                    text: line.model.who
                }

                Label {
                    x: 148
                    width: parent.width - 148 - 88
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: line.model.body
                }

                Button {
                    x: parent.width - 84
                    y: 1
                    width: 76
                    height: parent.height - 2
                    visible: Session.hasScope("admin")
                    text: qsTr("Erase")
                    onClicked: Server.erase(line.model.id)
                }
            }
        }

        TextField {
            id: draft

            Layout.fillWidth: true
            placeholderText: qsTr("Say something")
            onAccepted: {
                Server.say(draft.text);
                draft.clear();
            }
        }
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="import SynQt" data-href="runtime-api/">Brings in the runtime accessors: Server, Session, Router, and the contracts this entity consumes.</li>
<li data-code="ApplicationWindow {" data-href="programming-model/">One window, and signing in swaps what is in it and nothing else. There is no second page and no redirect, because the room's point is gated `scope: user` and a signed-out session has no `Server` to reach.</li>
<li data-code="visible: !Session.hasScope(&quot;user&quot;)" data-href="runtime-api/">The sign-in page, and the whole of what a signed-out visitor has. It is a binding, so it lifts by itself the moment the session is elevated.</li>
<li data-code="onClicked: Session.login()" data-href="authentication/">The flow runs on the edge. This browser never sees a token and never holds a secret; what it ends up with is a session cookie.</li>
<li data-code="model: Server.messages" data-href="programming-model/">A live model, and the whole of the sync. Somebody says something, the owner replaces the rows, and every open tab redraws itself. Nothing here polls.</li>
<li data-code="required property var model" data-href="programming-model/">A delegate is recycled, so it holds no state of its own. `var model` rather than a property per role, because one of the roles is called `id`, which is a QML keyword.</li>
<li data-code="line.model.staff" data-href="security/">Not a decision this browser made. `staff` is stamped on the row by the edge, from the session it verified, and it is the only thing in the system that can set it.</li>
<li data-code="visible: Session.hasScope(&quot;admin&quot;)" data-href="runtime-api/">A courtesy, not a gate: `erase` is not a member of the surface an ordinary session acquired, so hiding the button is only about not offering it.</li>
<li data-code="Server.say(draft.text)" data-href="api/?p=classSynQt_1_1ServerAccessor.html">A request, not a command. It runs on the owner, which is free to refuse it. The browser sends the text and nothing else.</li>
</ul>

</div>

<div class="synqt-file" data-file="edge" markdown>
<span class="synqt-file__name"><strong>edge</strong><span class="synqt-flow__path">web/edge/Edge.qml</span></span>

```qml
import SynQt

Edge {
    messagesRows: Store.lines

    function say(body) {
        Store.say(Caller.identity.login, body, Caller.hasScope("admin"));
    }

    function erase(id) {
        Store.erase(id);
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Edge {" data-href="programming-model/">The Source of the point this entity owns. Everything about a message except its text is decided here. The type is the entity's own name capitalised, because the owner names the point.</li>
<li data-code="messagesRows: Store.lines" data-href="programming-model/">Bind the model once and the room is live. Every browser holds a mirror of it, so reassigning the list on the database redraws all of them, and nobody wrote a broadcast.</li>
<li data-code="Caller.identity.login" data-href="api/?p=classSynQt_1_1Caller.html">Who is asking, taken from the session this edge verified. A browser cannot read this value, let alone set it, so `say` takes a line of text and no name.</li>
<li data-code="Caller.hasScope(&quot;admin&quot;)" data-href="security/">The one place in the system that can mark a message as staff. A user cannot speak in a moderator's voice by asking to, because nothing they send reaches this argument.</li>
<li data-code="function erase(id)" data-href="programming-model/">Only a caller who holds `admin` reaches this, because the contract says so. There is no check here refusing anybody, because nobody else has the slot.</li>
</ul>

</div>

<div class="synqt-file" data-file="database" markdown>
<span class="synqt-file__name"><strong>database</strong><span class="synqt-flow__path">db/relational/store/Store.qml</span></span>

```qml
import SynQt

Store {
    id: log

    function say(who, body, staff) {
        Db.exec("INSERT INTO messages (who, body, staff, said_at) "
                + "VALUES (?, ?, ?, datetime('now'))",
                [who, body, staff ? 1 : 0]);
        log.refresh();
    }

    function erase(id) {
        Db.exec("UPDATE messages SET body = 'deleted by a moderator' WHERE id = ?",
                [id]);
        log.refresh();
    }

    function refresh() {
        const rows = Db.query("SELECT id, who, body, staff FROM messages "
                              + "ORDER BY id DESC LIMIT 50");
        log.lines = rows.map(row => ({ id: row.id, who: row.who, body: row.body,
                                       staff: row.staff !== 0 }));
    }

    lines: []

    Component.onCompleted: log.refresh()
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Store {" data-href="programming-model/">The Source of the point the database owns, and the only surface it has. The conversation lives here, and it is the only thing in the system that survives a restart.</li>
<li data-code="Db.exec" data-href="providers/">Parameterized, always. The values travel beside the statement, so an apostrophe in a message is an apostrophe and never a second statement. Writes are serialized on this entity's own event loop.</li>
<li data-code="UPDATE messages SET body" data-href="programming-model/">Erasing leaves the line where it was, saying who wrote it and that it is gone, rather than opening a hole in a conversation everybody is reading.</li>
<li data-code="function refresh()" data-href="programming-model/">Read the room and hand it over, in one go. Called after every write, and once at startup, so there is one path by which the room changes and every window on it redraws from that one path.</li>
<li data-code="Db.query" data-href="providers/">`said_at` is in the table and not in this SELECT, and not in the contract either. The boundary keeps the declared roles and drops the rest.</li>
<li data-code="lines: []" data-href="programming-model/">The room as it stands, held once and reassigned in one go. That reassignment is the whole of the synchronisation.</li>
</ul>

</div>

<div class="synqt-file" data-file="schema" markdown>
<span class="synqt-file__name"><strong>table</strong><span class="synqt-flow__path">db/relational/store/schema.sql</span></span>

```sql
CREATE TABLE IF NOT EXISTS messages (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    who     TEXT NOT NULL,
    body    TEXT NOT NULL,
    staff   INTEGER NOT NULL DEFAULT 0,
    said_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS messages_by_time
    ON messages (said_at);
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="CREATE TABLE IF NOT EXISTS" data-href="providers/">Applied at startup and forward only. A migration adds a file. Nothing here is ever rewritten under a running system.</li>
<li data-code="said_at" data-href="security/">In the table and not in the contract, so it stays on this side of the mesh. A column a browser is not told about is a column it never receives.</li>
</ul>

</div>

<p class="synqt-flow__hint" aria-live="polite"></p>
<p class="synqt-flow__note">Hover a marked line for what it does. A line ending in an arrow opens the page covering it.</p>

</div>


</div>

</div>

<div class="synqt-actions">
<a class="cta cta--quiet" href="/designer/#example=demo" markdown="0"><span class="span">Open this project in the online designer</span></a>
</div>

</div>

<div class="synqt-section" markdown>

## Where to go next

- [Getting started](getting-started.md): install `synqt` and run your first
  project.
- [Framework](architecture.md): the full reference, from the entity model to the
  security design.
- [Examples](examples.md): complete worked systems.
- [Contributing](development.md): the codebase map, for working on SynQt itself.
- [C++ reference](api.md): the generated class and member reference for the
  runtime.

</div>

</div>
