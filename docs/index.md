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

A visitor who has signed in as nobody is served the landing page, and the room's client is
not hidden from them so much as absent: it is a file their session cannot fetch. Once they
have signed in, the edge stops answering for itself. It is drawn as a wedge because it is a
front: it keeps the session and the sign-in, and hands each caller to the entity that serves
people of their scope, which is why there are two surfaces behind it and a seat on its back
for each. A moderator lands on the one where `erase` exists, and an ordinary user lands on
the one where it does not exist at all. Neither of them holds the room: the database does,
and both mirror it, so there is one conversation however many surfaces stand in front of it.

Seven files are the whole system: one configuration file, which says what crosses each link,
one QML file per entity that implements something, and the table the database keeps the
messages in. The edge has none, and that is the example rather than an omission: it
implements no part of the point it owns. Hover (or focus) an entity to read the file it is,
or the mark on a line to read the block that says what crosses it, and the card that opens
says the rest. The project tree under the drawing opens the same files, and one stays open
until you move to another. The database opens two, its QML and the table that QML queries,
since neither says much without the other, and a directory in the tree opens everything in
it. Hover any line of a file to see what that line does, and a line that ends in an arrow
opens the page covering it, whether that is a page of this guide or the class in the C++
reference.

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
     data-files='{"entity:home": "home", "entity:app": "client",
                  "entity:edge": "config", "entity:room": "room",
                  "entity:moderation": "moderation",
                  "entity:store": "database schema",
                  "contract:edge": "config", "contract:room": "config",
                  "contract:moderation": "config",
                  "contract:store": "config"}'>
</div>
</div>

</div>

<div class="synqt-explorer__view">
<div class="synqt-tree">
<span class="synqt-tree__title">Project tree</span>
<ul class="synqt-tree__list">
<li class="synqt-tree__leaf"><span class="synqt-tree__file" data-file="config" tabindex="0" role="button" aria-label="Show synqt.yaml">synqt.yaml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="home client" tabindex="0" role="button" aria-label="Show both client entities">client</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="home" tabindex="0" role="button" aria-label="Show client/home/Main.qml">home</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="home" tabindex="0" role="button" aria-label="Show client/home/Main.qml">Main.qml</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">app</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">Main.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="room" tabindex="0" role="button" aria-label="Show the cache entity">cache</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="room" tabindex="0" role="button" aria-label="Show cache/room/Room.qml">room</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="room" tabindex="0" role="button" aria-label="Show cache/room/Room.qml">Room.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="moderation" tabindex="0" role="button" aria-label="Show the service entity">service</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="moderation" tabindex="0" role="button" aria-label="Show service/moderation/Moderation.qml">moderation</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="moderation" tabindex="0" role="button" aria-label="Show service/moderation/Moderation.qml">Moderation.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="database schema" tabindex="0" role="button" aria-label="Show the relational entities">db/relational</span></li>
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
  - { name: home, type: client }
  - { name: app, type: client }
  - name: edge
    type: web_edge
    identity: true
    public: { port: 8443, sync_route: /sync }
    bundles: { anonymous: home, user: app }
  - { name: room, type: cache, provider: { name: memory } }
  - { name: moderation, type: service }
  - { name: store, type: relational, provider: { name: sqlite } }

connect_points:
  - owner: edge
    consumers: [app]
    scope: user
    behind: { user: room, admin: moderation }
    export: |
      prop string[60] topic
      model messages(int id, string[40] who, string[280] body, bool staff)
      slot say(string[280] body)
      <admin> slot erase(int id)
      signal refused(string[120] reason)
  - owner: room
    consumers: [edge]
    export: |
      prop string[60] topic
      model messages(int id, string[40] who, string[280] body, bool staff)
      slot say(string[280] body)
      signal refused(string[120] reason)
  - owner: moderation
    consumers: [edge]
    export: |
      prop string[60] topic
      model messages(int id, string[40] who, string[280] body, bool staff)
      slot say(string[280] body)
      slot erase(int id)
      signal refused(string[120] reason)
  - owner: store
    consumers: [room, moderation]
    export: |
      prop string[60] topic
      prop var[24000] lines
      slot say(string[40] who, string[280] body, bool staff)
      slot erase(int id)
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="qt_version" data-href="build-system-and-cli/">One version pins the whole toolchain: Qt, the Emscripten it is built against, and every entity built from them.</li>
<li data-code="order: [anonymous" data-href="security/">The scope ladder. Every session sits on one rung, and a connect point can demand a minimum.</li>
<li data-code="mapping: web/edge/identity/map.qml" data-href="authentication/">The one place anybody is decided to be a moderator. It turns a verified login into a scope, and everything else in the system reads that answer rather than making it.</li>
<li data-code="name: home" data-href="security/">The landing page, and the only bundle a signed-out visitor is ever sent. It consumes nothing, so there is nothing on it to attack.</li>
<li data-code="name: app" data-href="desktop/">The room itself, built to WebAssembly. The same QML also builds as a native app for Windows, macOS, and Linux, against this same edge.</li>
<li data-code="type: web_edge" data-href="entities/">The one entity allowed to face the internet, on the one public port. Nothing else gets one.</li>
<li data-code="identity: true" data-href="authentication/">The switch the whole project hangs off: this edge runs the OAuth exchange and keeps the sessions, which is what makes `Session.login()` in a client reach anything at all.</li>
<li data-code="bundles: { anonymous: home" data-href="security/">The delivery gate. A visitor is served the bundle their scope maps to and no file of any other, so the room's client is not on a signed-out visitor's disk at all: not a redirect, not a 403, simply not there.</li>
<li data-code="name: room, type: cache" data-href="providers/">A bounded key-value store that forgets. It serves signed-in users, and the one thing it keeps of its own is how much each of them has said lately.</li>
<li data-code="name: moderation" data-href="entities/">An entity with no engine: its own logic, its own binary. It is what a moderator is handed to, and `erase` is compiled into it and into nothing else.</li>
<li data-code="type: relational" data-href="providers/">A database entity: embedded SQLite by default, PostgreSQL or MySQL behind the same interface with one config value.</li>
<li data-code="scope: user" data-href="programming-model/">The gate on the whole point. A session that has signed in as nobody never acquires it, so there is no surface for them to reach.</li>
<li data-code="behind: { user: room" data-href="programming-model/">The edge answers none of this point. It keeps the session and the sign-in and hands each caller to the entity that serves people of their scope; the browser writes `Server` whichever one answered.</li>
<li data-code="consumers: [app]" data-href="project-layout-and-config/">The browser's one way in, and deny by default: an entity that is not on this list cannot open this connect point at all.</li>
<li data-code="&lt;admin&gt; slot erase(int id)" data-href="security/">The gate is on the member. A caller without the scope does not have the slot, so it is refused before it runs and there is no check to write, or forget, in the QML behind it.</li>
<li data-code="model messages(int id" data-href="programming-model/">The roles listed here are the whole of what a message may carry to a browser. `said_at` is in the table and not in this line, so it never leaves the mesh.</li>
<li data-code="consumers: [room, moderation]" data-href="entities/">The database is reachable by the two surfaces in front of it and by nothing else. The browser is on no list here, so there is no request it can make.</li>
<li data-code="prop var[24000] lines" data-href="programming-model/">The room, held in one place and mirrored by both surfaces. The bracketed number is the limit the owner holds it to at the boundary, in bytes on the wire.</li>
</ul>

</div>

<div class="synqt-file" data-file="home" markdown>
<span class="synqt-file__name"><strong>home</strong><span class="synqt-flow__path">client/home/Main.qml</span></span>

```qml
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

// The landing page, and the whole of what a visitor who has signed in as nobody downloads.
// The room's client is a different bundle on the same edge, and a session without `user`
// cannot fetch a file of it: not a redirect, not a 403, simply not there.
//
// There is no `Server` here and nothing to reach for. This entity consumes no connect point,
// so the only thing this application can do is start the sign-in, which is the only thing
// somebody who is nobody yet has any business doing.
ApplicationWindow {
    id: window

    visible: true
    title: qsTr("The chat room")

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 24

        Label {
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 32
            text: qsTr("One room. Everybody in it sees the same thing.")
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 480
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Somebody types a line and it is on every screen that has the room "
                       + "open, including the ones on other machines. Sign in to join.")
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            // The edge runs the whole exchange. This browser never sees a token and never
            // holds a secret; what it ends up with is a session cookie, and the next page
            // it is served is the room.
            text: qsTr("Sign in with GitHub")
            onClicked: Session.login()
        }
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="ApplicationWindow" data-href="project-layout-and-config/">A client's Main.qml is the window. A root that is not a window builds fine and renders nothing.</li>
<li data-code="Session.login()" data-href="authentication/">The whole of what this bundle can do. The flow runs on the edge, and this browser ends up holding a session cookie and nothing else.</li>
</ul>

</div>

<div class="synqt-file" data-file="client" markdown>
<span class="synqt-file__name"><strong>app</strong><span class="synqt-flow__path">client/app/Main.qml</span></span>

```qml
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

// The room. One client for users and moderators both: what a moderator can do extra is
// decided at the edge and by the entity behind it, never by which files a browser was given.
// The Erase button below is a courtesy, not a gate -- an ordinary user's session was handed
// to an entity whose surface has no `erase` on it at all.
ApplicationWindow {
    id: window

    property string notice: ""

    visible: true
    // A property the owner pushes. It is set once, on the database, and arrives here through
    // the entity serving this caller and then the edge: three hops, no polling, one value.
    title: Server.topic

    // What the owner says back when it says no, to the caller that asked and to nobody else
    // in the room.
    Edge.onRefused: reason => window.notice = reason

    ColumnLayout {
        anchors.fill: parent

        ListView {
            id: messages

            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            model: Server.messages

            // Simple x/width bindings rather than a layout, which is what a delegate wants:
            // it is created and destroyed as the view scrolls, and `model` is read through a
            // required property because a role called `id` cannot be one of its own.
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
                    // A moderator's name is red, and nothing in this browser decided that.
                    // `staff` is stamped on the row by the entity a moderator is handed to,
                    // which is the only place in the system that can set it.
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
                    // Shown to a moderator because there is no sense offering it to anybody
                    // else. It is not what stops anybody else: `erase` is not a member of the
                    // surface an ordinary session acquired, so a console call finds nothing.
                    visible: Session.hasScope("admin")
                    text: qsTr("Erase")
                    onClicked: Server.erase(line.model.id)
                }
            }
        }

        TextField {
            id: draft

            Layout.fillWidth: true
            placeholderText: window.notice || qsTr("Say something")
            onAccepted: {
                Server.say(draft.text);
                window.notice = "";
                draft.clear();
            }
        }
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="import SynQt" data-href="runtime-api/">Brings in the runtime accessors: Server, Session, Router, and the contracts this entity consumes.</li>
<li data-code="title: Server.topic" data-href="programming-model/">The contract's property, arriving from the entity that answered this caller. Read-only here, and live.</li>
<li data-code="Edge.onRefused" data-href="api/?p=classSynQt_1_1ConsumerBase.html">The contract's signal, handled where it arrives. The edge names its own point, so `Edge` is what the handler attaches to, whichever entity behind it raised the signal.</li>
<li data-code="model: Server.messages" data-href="programming-model/">A live model, and the whole of the sync. Somebody says something, the owner replaces the rows, and every open tab redraws itself. Nothing here polls.</li>
<li data-code="required property var model" data-href="programming-model/">A delegate is recycled, so it holds no state of its own. `var model` rather than a property per role, because one of the roles is called `id`, which is a QML keyword.</li>
<li data-code="line.model.staff" data-href="security/">Not a decision this browser made. `staff` is stamped on the row by the entity a moderator is handed to, and it is the only thing in the system that can set it.</li>
<li data-code="Session.hasScope" data-href="runtime-api/">A courtesy, not a gate: `erase` is not a member of the surface an ordinary session acquired, so hiding the button is only about not offering it.</li>
<li data-code="Server.say(draft.text)" data-href="api/?p=classSynQt_1_1ServerAccessor.html">A request, not a command. It runs on the owner, which is free to refuse it.</li>
</ul>

</div>

<div class="synqt-file" data-file="room" markdown>
<span class="synqt-file__name"><strong>room</strong><span class="synqt-flow__path">cache/room/Room.qml</span></span>

```qml
import SynQt

// What a signed-in user is handed to. Nothing here asks about scope, and that is not an
// omission: the edge in front of it hands nobody but a `user` here, so `Caller` is the only
// question there is to ask, and the members a moderator reaches are not on this surface for
// anybody to call.
//
// The room itself is not here either. `store` holds it, this mirrors it, and the moderator's
// entity mirrors the same one, which is what makes them two views of one conversation.
Room {
    id: room

    function say(body) {
        const who = Caller.identity.login;
        // How much this person has said in the last minute, kept in the one place worth
        // keeping it: a bounded store that forgets. The window starts when the first message
        // of it lands, so this is a fixed minute rather than a minute from the last thing
        // said, which would never expire for somebody typing steadily.
        const said = Cache.incr("said:" + who);
        if (said === 1) {
            Cache.expire("said:" + who, 60);
        }
        if (said > 20) {
            Caller.emitRefused("Twenty lines a minute is the limit. Give it a moment.");
            return;
        }
        Store.say(who, body, false);
    }

    topic: Store.topic
    messagesRows: Store.lines
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Room {" data-href="programming-model/">The Source of the point this entity owns. The type is the entity's own name capitalised.</li>
<li data-code="topic: Store.topic" data-href="programming-model/">One value, three hops. The database sets it, this mirrors it, and the edge relays it to the browser, with nothing polling anywhere along the way.</li>
<li data-code="messagesRows: Store.lines" data-href="programming-model/">Bind the model once and the room is live. Every browser handed to this entity holds a mirror of it, so reassigning the list on the database redraws all of them.</li>
<li data-code="Caller.identity.login" data-href="api/?p=classSynQt_1_1Caller.html">Who is asking, taken from the session the edge verified and handed down with the call. A browser cannot read this value, let alone set it.</li>
<li data-code="Cache.incr" data-href="providers/">A bounded store that forgets, which is exactly what a rate counter wants. In process by default, Redis behind the same interface with one config value.</li>
<li data-code="Caller.emitRefused" data-href="runtime-api/">The contract's signal, emitted to this caller alone. Everybody else in the room sees nothing.</li>
</ul>

</div>

<div class="synqt-file" data-file="moderation" markdown>
<span class="synqt-file__name"><strong>moderation</strong><span class="synqt-flow__path">service/moderation/Moderation.qml</span></span>

```qml
import SynQt

// What a moderator is handed to, in a binary of its own. `erase` is compiled into this
// entity and into nothing else, and neither is the line below that marks a message as staff:
// the process serving ordinary users does not contain either of them.
//
// Like the entity next door, it never asks about scope. The edge decides who arrives here.
Moderation {
    id: desk

    // A moderator is not rate limited, which is why this entity has no cache and the one
    // serving users does. What is different about the two surfaces is what each of them
    // holds, not a flag either of them reads.
    function say(body) {
        Store.say(Caller.identity.login, body, true);
    }

    // Read against the room as it stands rather than against a query of its own: the caller
    // is looking at these rows, so this is the question they think they are asking. The
    // answer goes back to the one caller who asked it and to nobody else in the room.
    function erase(id) {
        if (!Store.lines.some(line => line.id === id)) {
            Caller.emitRefused("That message is not in the room any more.");
            return;
        }
        Store.erase(id);
    }

    topic: Store.topic
    messagesRows: Store.lines
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Moderation {" data-href="programming-model/">The other half of the front: the same surface a user reaches, plus what only a moderator does. Its own entity, so its own binary.</li>
<li data-code="Store.say(Caller.identity.login, body, true)" data-href="security/">The `true` is what marks a message as staff, and this line is compiled into this entity and nowhere else. The process serving ordinary users does not contain it.</li>
<li data-code="function erase(id)" data-href="programming-model/">Not on the surface a user acquired at all. There is no check here refusing them, because there is nothing for them to call.</li>
</ul>

</div>

<div class="synqt-file" data-file="database" markdown>
<span class="synqt-file__name"><strong>database</strong><span class="synqt-flow__path">db/relational/store/Store.qml</span></span>

```qml
import SynQt

// The conversation, and the only thing here that survives a restart. Nothing in this file
// asks who is calling: this point lists two consumers, so those two entities are the only
// ones that can acquire it at all, and a browser is on no consumer list anywhere.
Store {
    id: log

    function say(who, body, staff) {
        Db.exec("INSERT INTO messages (who, body, staff, said_at) "
                + "VALUES (?, ?, ?, datetime('now'))",
                [who, body, staff ? 1 : 0]);
        log.refresh();
    }

    function erase(id) {
        Db.exec("DELETE FROM messages WHERE id = ?", [id]);
        log.refresh();
    }

    // The room as it stands, reassigned in one go. Every surface in front of this mirrors
    // the property, so one line typed anywhere redraws every window open on the room and
    // nobody wrote a broadcast.
    //
    // `said_at` is in the table and not in the SELECT, and not in the contract either. A
    // column the browser is never told about is a column it cannot receive: the boundary
    // keeps the declared roles and drops the rest, so it could not cross even by accident.
    function refresh() {
        const rows = Db.query("SELECT id, who, body, staff FROM messages "
                              + "ORDER BY id DESC LIMIT 50");
        log.lines = rows.map(row => ({ id: row.id, who: row.who, body: row.body,
                                       staff: row.staff !== 0 }));
    }

    topic: "Anything goes"
    lines: []

    Component.onCompleted: log.refresh()
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Store {" data-href="programming-model/">The Source of the point the database owns, and the only surface it has. There is no other way in.</li>
<li data-code="lines: []" data-href="programming-model/">The room as it stands, held once. Both surfaces in front of it mirror this one property, which is what makes them two views of one conversation.</li>
<li data-code="Db.exec" data-href="providers/">Parameterized, always. The values travel beside the statement, so an apostrophe in a message is an apostrophe and never a second statement. Writes are serialized on this entity's own event loop.</li>
<li data-code="Db.query" data-href="providers/">`said_at` is in the table and not in this SELECT, and not in the contract either: the boundary keeps the declared roles and drops the rest.</li>
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
<p class="synqt-flow__note">Hover a line for what it does. A line ending in an arrow opens the page covering it.</p>

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
