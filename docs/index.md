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

A finished system is a small mesh of entities. Only the web edge faces the internet;
everything else is private and reachable only by the entities you allow. Here a signed-out
visitor is served a sign-in page and nothing else, a signed-in reader is served the room,
and the room's one request goes to the web edge, which decides whether that caller may
speak and hands the line to the database. The reply is a model, so every browser holding
the room redraws itself; nobody wrote any of that.

Six files are the whole system: one configuration file, which says what crosses each link,
one QML file per entity, and the table the database keeps the messages in. Hover (or focus)
any part of the diagram to read the file behind it, or pick the file out of the project tree
beside it; it stays open until you move to another one. The database opens two, its QML and
the table that QML queries, since neither says much without the other, and a directory in
the tree opens everything in it. Hover any line of a file to see what that line does, and a
line that ends in an arrow opens the page covering it, whether that is a page of this guide
or the class in the C++ reference.

The button under the tree opens this same system in the
[online designer](visual-editor.md), which runs in the browser with nothing installed. Pull
the mesh apart there, add an entity, and download the result as a project.

<div class="synqt-explorer">

<div class="synqt-mesh">

<div class="synqt-flow">
<div class="synqt-flow__stage">
<svg viewBox="0 32 460 232" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
  <defs>
    <filter id="synqt-flow-glow" x="-100%" y="-100%" width="300%" height="300%">
      <feGaussianBlur stdDeviation="5" result="blur"/>
      <feMerge>
        <feMergeNode in="blur"/>
        <feMergeNode in="SourceGraphic"/>
      </feMerge>
    </filter>
  </defs>

  <!-- Delivery, not a link. The edge hands a signed-out visitor this bundle and that
       browser holds no connect point at all, which is the difference the gate is here to
       show, so it is drawn as a different kind of line. -->
  <g class="synqt-flow__lines synqt-flow__lines--served">
    <line x1="75.9" y1="84.1" x2="195.8" y2="130.6"/>
  </g>

  <g class="synqt-flow__lines">
    <line x1="78.6" y1="194.8" x2="195.8" y2="149.4"/>
    <line x1="246.0" y1="140.0" x2="372.0" y2="140.0"/>
  </g>

  <g class="synqt-flow__edges">
    <text x="140" y="98" text-anchor="middle">served the gate</text>
    <text x="140" y="200" text-anchor="middle">the room</text>
    <text x="305" y="158" text-anchor="middle">the messages</text>
  </g>

  <!-- Both links are encrypted and authenticated: wss for the browser, mutual TLS
       against a private CA between entities (docs/security.md). Static rather than tied
       to the packet, because it is true between requests too. -->
  <g class="synqt-flow__locks">
    <g transform="translate(159.2,163.6) scale(0.5)" fill="none" stroke="#9a94c4" stroke-width="1.6">
      <title>wss (TLS)</title>
      <path d="M -4,-1 v -3 a 4,4 0 0 1 8,0 v 3"/>
      <rect x="-6" y="-1" width="12" height="9" rx="1.5" fill="#9a94c4" stroke="none"/>
    </g>
    <g transform="translate(305.0,140.0) scale(0.5)" fill="none" stroke="#9a94c4" stroke-width="1.6">
      <title>mutual TLS</title>
      <path d="M -4,-1 v -3 a 4,4 0 0 1 8,0 v 3"/>
      <rect x="-6" y="-1" width="12" height="9" rx="1.5" fill="#9a94c4" stroke="none"/>
    </g>
  </g>

  <!-- One contract per connect point, drawn on the link it is shared across, because
       that is what a connect point is. The gate's line carries none: it is served a
       bundle and consumes nothing. Both open the configuration, which is where a link's
       shape is written. -->
  <g transform="translate(186,172) scale(1.5)" fill="none" stroke="#e5e7ff" stroke-width="0.9">
    <rect x="-4" y="-5" width="8" height="10" rx="1"/>
    <line x1="-2" y1="-1.5" x2="2" y2="-1.5"/>
    <line x1="-2" y1="1" x2="2" y2="1"/>
  </g>
  <g transform="translate(305,118) scale(1.5)" fill="none" stroke="#e5e7ff" stroke-width="0.9">
    <rect x="-4" y="-5" width="8" height="10" rx="1"/>
    <line x1="-2" y1="-1.5" x2="2" y2="-1.5"/>
    <line x1="-2" y1="1" x2="2" y2="1"/>
  </g>

  <!-- The configuration, drawn and named the way the entities are. It sits beside the
       mesh rather than in it, because it is the file the whole drawing is generated
       from rather than a member of it. -->
  <g transform="translate(300,216) scale(0.62) translate(-12,-12)" fill="#e5e7ff">
    <g class="synqt-flow__cog">
    <path d="M12,15.5A3.5,3.5 0 0,1 8.5,12A3.5,3.5 0 0,1 12,8.5A3.5,3.5 0 0,1 15.5,12A3.5,3.5 0 0,1 12,15.5M19.43,12.97C19.47,12.65 19.5,12.33 19.5,12C19.5,11.67 19.47,11.34 19.43,11L21.54,9.37C21.73,9.22 21.78,8.95 21.66,8.73L19.66,5.27C19.54,5.05 19.27,4.96 19.05,5.05L16.56,6.05C16.04,5.66 15.5,5.32 14.87,5.07L14.5,2.42C14.46,2.18 14.25,2 14,2H10C9.75,2 9.54,2.18 9.5,2.42L9.13,5.07C8.5,5.32 7.96,5.66 7.44,6.05L4.95,5.05C4.73,4.96 4.46,5.05 4.34,5.27L2.34,8.73C2.21,8.95 2.27,9.22 2.46,9.37L4.57,11C4.53,11.34 4.5,11.67 4.5,12C4.5,12.33 4.53,12.65 4.57,12.97L2.46,14.63C2.27,14.78 2.21,15.05 2.34,15.27L4.34,18.73C4.46,18.95 4.73,19.03 4.95,18.95L7.44,17.94C7.96,18.34 8.5,18.68 9.13,18.93L9.5,21.58C9.54,21.82 9.75,22 10,22H14C14.25,22 14.46,21.82 14.5,21.58L14.87,18.93C15.5,18.67 16.04,18.34 16.56,17.94L19.05,18.95C19.27,19.03 19.54,18.95 19.66,18.73L21.66,15.27C21.78,15.05 21.73,14.78 21.54,14.63L19.43,12.97Z"/>
    </g>
  </g>

  <!-- One trip: somebody says something, the edge decides whether they may, the database
       keeps it, and the answer goes back. The ball travels behind the entities (this
       group comes before .synqt-flow__nodes, and SVG paints in document order), so it
       passes under each circle instead of over it. -->
  <g class="synqt-flow__packets">
    <circle r="4.5" fill="#46f477">
      <animateMotion dur="7s" begin="0s" repeatCount="indefinite" calcMode="linear"
        keyPoints="0;1;1" keyTimes="0;0.86;1"
        path="M60,202 L220,140 L390,140 L220,140 L60,202"/>
      <animate attributeName="fill" dur="7s" begin="0s" repeatCount="indefinite"
        values="#46f477;#46f477;#8890c0;#8890c0;#46f477;#46f477"
        keyTimes="0;0.216;0.216;0.644;0.644;1"/>
    </circle>
  </g>

  <g class="synqt-flow__nodes">
    <circle cx="60" cy="78" r="17" class="synqt-flow__node synqt-flow__node--user" filter="url(#synqt-flow-glow)"/>
    <circle cx="60" cy="202" r="20" class="synqt-flow__node synqt-flow__node--user" filter="url(#synqt-flow-glow)"/>
    <circle cx="220" cy="140" r="26" class="synqt-flow__node synqt-flow__node--hub" filter="url(#synqt-flow-glow)"/>
    <circle cx="390" cy="140" r="18" class="synqt-flow__node synqt-flow__node--service" filter="url(#synqt-flow-glow)"/>
  </g>

  <!-- Each entity's own permanent glyph: what it is, wherever the request happens to be.
       Explicit fill/stroke on every shape rather than inherited from a class on the
       wrapping <g>: an SVG presentation attribute loses to a CSS rule targeting that same
       element, so a parent <g fill="none"> does not reliably keep a styled child
       transparent. -->
  <g class="synqt-flow__static-icons">
    <g transform="translate(60,78)" fill="#46f477">
      <circle cx="0" cy="-3.2" r="3.2"/>
      <path d="M -6,7.5 a 6,6.5 0 0 1 12,0 z"/>
    </g>
    <g transform="translate(60,202)" fill="#46f477">
      <circle cx="0" cy="-3.2" r="3.2"/>
      <path d="M -6,7.5 a 6,6.5 0 0 1 12,0 z"/>
    </g>
    <g transform="translate(390,140)" fill="none" stroke="#8890c0" stroke-width="1.4">
      <ellipse cx="0" cy="-4.5" rx="6.5" ry="2.2" fill="#8890c0" stroke="none"/>
      <path d="M -6.5,-4.5 V 4.5 A 6.5,2.2 0 0 0 6.5,4.5 V -4.5"/>
      <path d="M -6.5,0 A 6.5,2.2 0 0 0 6.5,0"/>
    </g>
  </g>

  <!-- The edge's own state through one trip: nothing at rest, a caller once the request
       arrives, a tick once it has decided that caller may, and the message once the
       database has kept it. Every timing is a fraction of the packet's own path length
       above, so each change happens as the packet lands. -->
  <g class="synqt-flow__hub-icons">
    <g transform="translate(215,138)" fill="#46f477" opacity="0">
      <circle cx="0" cy="-3.2" r="3.2"/>
      <path d="M -6,7.5 a 6,6.5 0 0 1 12,0 z"/>
      <animate attributeName="opacity" dur="7s" begin="0s" repeatCount="indefinite"
        values="0;0;1;1;0" keyTimes="0;0.216;0.216;0.86;1"/>
    </g>
    <g transform="translate(229,132)" fill="none" stroke="#e6b450" stroke-width="1.6" stroke-linecap="round" opacity="0">
      <path d="M -2,-2.6 A 2.6,2.6 0 1 1 0,1.4"/>
      <path d="M 0,4 v 0.6"/>
      <animate attributeName="opacity" dur="7s" begin="0s" repeatCount="indefinite"
        values="0;0;1;1;0;0" keyTimes="0;0.216;0.216;0.43;0.43;1"/>
    </g>
    <g transform="translate(229,132)" fill="none" stroke="#46f477" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" opacity="0">
      <path d="M -3,0.4 L -0.8,2.6 L 3.4,-2.4"/>
      <animate attributeName="opacity" dur="7s" begin="0s" repeatCount="indefinite"
        values="0;0;1;1;0" keyTimes="0;0.43;0.43;0.86;1"/>
    </g>
    <g transform="translate(229,150)" fill="none" stroke="#e5e7ff" stroke-width="1.3" opacity="0">
      <rect x="-5" y="-3.5" width="10" height="7" rx="1"/>
      <path d="M -5,-3.5 L 0,0.6 L 5,-3.5"/>
      <animate attributeName="opacity" dur="7s" begin="0s" repeatCount="indefinite"
        values="0;0;1;1;0" keyTimes="0;0.644;0.644;0.86;1"/>
    </g>
  </g>

  <g class="synqt-flow__labels">
    <text x="60" y="110" text-anchor="middle">gate</text>
    <text x="60" y="238" text-anchor="middle">app</text>
    <text x="220" y="182" text-anchor="middle">edge</text>
    <text x="390" y="174" text-anchor="middle">store</text>
    <text x="300" y="240" text-anchor="middle">configuration</text>
  </g>

  <g class="synqt-flow__kinds">
    <text x="60" y="123" text-anchor="middle">client</text>
    <text x="60" y="251" text-anchor="middle">client</text>
    <text x="220" y="195" text-anchor="middle">web edge</text>
    <text x="390" y="187" text-anchor="middle">relational</text>
  </g>
</svg>

<div class="synqt-flow__hotspot synqt-flow__hotspot--gate" data-file="gate" tabindex="0" role="button" aria-label="Show client/gate/Main.qml"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--app" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--edge" data-file="web" tabindex="0" role="button" aria-label="Show web/edge/Edge.qml"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--store" data-file="database schema" tabindex="0" role="button" aria-label="Show the store entity's two files"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--config" data-file="config" tabindex="0" role="button" aria-label="Show synqt.yaml"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--edge-contract" data-file="config" tabindex="0" role="button" aria-label="Show what the edge connect point carries"></div>
<div class="synqt-flow__hotspot synqt-flow__hotspot--store-contract" data-file="config" tabindex="0" role="button" aria-label="Show what the store connect point carries"></div>
</div>
</div>

</div>

<div class="synqt-explorer__view">

<div class="synqt-explorer__files">

<div class="synqt-file" data-file="config" markdown>
<span class="synqt-file__name"><strong>configuration</strong><span class="synqt-flow__path">synqt.yaml</span></span>

```yaml
project:
  name: demo
  qt_version: 6.11.1

scopes: { order: [anonymous, user, admin], default: anonymous }

entities:
  - { name: gate, type: client, edge: edge }
  - { name: app, type: client, edge: edge }
  - name: edge
    type: web_edge
    public: { port: 8443, sync_route: /sync }
    bundles: { anonymous: gate, user: app }
  - { name: store, type: relational }

connect_points:
  - owner: edge
    consumers: [app]
    export: |
      prop string[60] topic
      model messages(int id, string[40] who, string[280] body)
      slot say(string[280] body)
      signal refused(string[120] reason)
  - owner: store
    consumers: [edge]
    export: |
      slot var recent()
      slot var append(string[40] who, string[280] body)
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="qt_version" data-href="build-system-and-cli/">One version pins the whole toolchain: Qt, the Emscripten it is built against, and every entity built from them.</li>
<li data-code="order: [anonymous" data-href="security/">The scope ladder. Every session sits on one rung, and a connect point can demand a minimum.</li>
<li data-code="name: gate" data-href="security/">A second client, and the only one a signed-out visitor is ever sent. It consumes nothing, so there is nothing on it to attack.</li>
<li data-code="name: app" data-href="desktop/">The room itself, built to WebAssembly. The same QML also builds as a native app for Windows, macOS, and Linux, against this same edge.</li>
<li data-code="type: web_edge" data-href="entities/">The one entity allowed to face the internet, on the one public port. Nothing else gets one.</li>
<li data-code="bundles: { anonymous: gate" data-href="security/">The delivery gate. A visitor is served the bundle their scope maps to and no file of any other, so the room's client is not on a signed-out visitor's disk at all: not a redirect, not a 403, simply not there.</li>
<li data-code="type: relational" data-href="providers/">A database entity: embedded SQLite by default, PostgreSQL or MySQL behind the same interface with one config value.</li>
<li data-code="consumers: [app]" data-href="project-layout-and-config/">The browser's one way in, and deny by default: an entity that is not on this list cannot open this connect point at all.</li>
<li data-code="consumers: [edge]" data-href="entities/">The database is reachable by the edge, over mutual TLS, and by nothing else. The browser is on no list here, so there is no request it can make.</li>
<li data-code="export: |" data-href="programming-model/">What may cross the link, and the whole of it. The owner names the type it exports: `edge` exports `Edge`, which is what both sides compile against.</li>
<li data-code="prop string[60] topic" data-href="programming-model/">Owner to consumers, pushed. Every window on this room retitles itself when it changes, and a consumer can read it but never set it.</li>
<li data-code="model messages(int id" data-href="programming-model/">The roles listed here are the whole of what a message may carry to a browser. `said_at` is in the table and not in this line, so it never leaves the mesh.</li>
<li data-code="slot say(string[280] body)" data-href="programming-model/">Consumer to owner: the one direction a request travels. The 280 is enforced by the owner at the boundary, not by the field that typed it.</li>
<li data-code="signal refused(string[120] reason)" data-href="programming-model/">The owner's answer when it says no, addressed to the caller that asked and to nobody else in the room.</li>
<li data-code="slot var recent()" data-href="programming-model/">A slot with a return type. The caller gets a promise, so the edge can wait on the answer without blocking anything else it is serving.</li>
</ul>

</div>

<div class="synqt-file" data-file="gate" markdown>
<span class="synqt-file__name"><strong>gate</strong><span class="synqt-flow__path">client/gate/Main.qml</span></span>

```qml
import SynQt
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    visible: true
    title: qsTr("Sign in")

    // This bundle is the whole of what a signed-out visitor downloads. The room's own
    // client is a different bundle on the same edge, and a session without `user` cannot
    // fetch a file of it: not a redirect, not a 403, simply not there.
    Column {
        anchors.centerIn: parent
        spacing: 16

        Label {
            text: qsTr("Sign in to join the room.")
        }

        Button {
            text: qsTr("Sign in")
            onClicked: Session.login()
        }
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="ApplicationWindow" data-href="project-layout-and-config/">A client's Main.qml is the window. A root that is not a window builds fine and renders nothing.</li>
<li data-code="Session.login()" data-href="authentication/">The whole of the gate. The flow runs on the edge, and this browser ends up holding a session cookie and nothing else.</li>
</ul>

</div>

<div class="synqt-file" data-file="client" markdown>
<span class="synqt-file__name"><strong>app</strong><span class="synqt-flow__path">client/app/Main.qml</span></span>

```qml
import SynQt
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    property string notice: ""

    visible: true
    // A property the edge pushes. Change it there and every window open on this room
    // retitles itself, with nothing here asking and nothing polling.
    title: Server.topic

    Edge.onRefused: reason => window.notice = reason

    ColumnLayout {
        anchors.fill: parent

        ListView {
            Layout.fillHeight: true
            Layout.fillWidth: true
            model: Server.messages

            delegate: Text {
                required property var model

                text: `${model.who}: ${model.body}`
            }
        }

        TextField {
            id: line

            Layout.fillWidth: true
            placeholderText: window.notice || qsTr("Say something")
            onAccepted: {
                Server.say(line.text);
                line.clear();
            }
        }
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="import SynQt" data-href="runtime-api/">Brings in the runtime accessors: Server, Session, Router, and the contracts this entity consumes.</li>
<li data-code="title: Server.topic" data-href="programming-model/">The contract's property, pushed by the edge. Read-only here, and live: change it on the edge and every window open on this room retitles itself.</li>
<li data-code="Edge.onRefused" data-href="api/?p=classSynQt_1_1ConsumerBase.html">The contract's signal, handled where it arrives. The edge names its own point, so `Edge` is what the handler attaches to. No Connections block, no target to wire up.</li>
<li data-code="model: Server.messages" data-href="programming-model/">A live model, and the whole of the sync. Somebody says something, the edge replaces the rows, and every open tab redraws itself. Nothing here polls.</li>
<li data-code="Server.say(line.text)" data-href="api/?p=classSynQt_1_1ServerAccessor.html">A request, not a command. It runs in the edge, which is free to refuse it.</li>
</ul>

</div>

<div class="synqt-file" data-file="web" markdown>
<span class="synqt-file__name"><strong>web edge</strong><span class="synqt-flow__path">web/edge/Edge.qml</span></span>

```qml
import SynQt

Edge {
    id: room

    property var said: []

    function say(body) {
        if (!Caller.hasScope("user")) {
            Caller.emitRefused("Sign in to say something.");
            return;
        }
        Store.append(Caller.identity.login, body).then(rows => room.said = rows);
    }

    topic: "Anything goes"
    // There is one of this entity, so there is one of this list: it is the room. Every
    // browser holds a mirror of this Source, so one person saying something redraws all
    // of them, and the only thing anybody wrote is the line below.
    messagesRows: room.said

    Component.onCompleted: Store.recent().then(rows => room.said = rows)
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Edge {" data-href="programming-model/">The Source of the point this entity owns. The type is the entity's own name capitalised, and there is one of it, because there is one of this entity.</li>
<li data-code="Caller.hasScope" data-href="api/?p=classSynQt_1_1Caller.html">Who is asking, decided by the edge from the session it holds. A browser cannot read this value, let alone set it.</li>
<li data-code="Caller.emitRefused" data-href="runtime-api/">The contract's signal, emitted to this caller alone. Everybody else in the room sees nothing.</li>
<li data-code="Store.append" data-href="programming-model/">The database, over mutual TLS, by name. The promise is the answer coming back; nothing blocks while it does.</li>
<li data-code="messagesRows: room.said" data-href="programming-model/">Bind the model once and the room is live. Every browser holds a mirror of this one Source, so assigning this list is what redraws all of them.</li>
</ul>

</div>

<div class="synqt-file" data-file="database" markdown>
<span class="synqt-file__name"><strong>database</strong><span class="synqt-flow__path">db/relational/store/Store.qml</span></span>

```qml
import SynQt

Store {
    id: log

    // Nothing here asks who is calling. This point lists one consumer, so the edge is
    // the only entity that can acquire it at all, and a browser is on no list anywhere.
    function recent() {
        return Db.query(
            "SELECT id, who, body FROM messages ORDER BY id DESC LIMIT 50");
    }

    function append(who, body) {
        Db.exec("INSERT INTO messages (who, body, said_at) VALUES (?, ?, datetime('now'))",
                [who, body]);
        return log.recent();
    }
}
```

<ul class="synqt-flow__glossary" hidden>
<li data-code="Store {" data-href="programming-model/">The Source of the point the database owns, and the only surface it has. There is no other way in.</li>
<li data-code="Db.query" data-href="providers/">Parameterized, always. The values travel beside the statement, so an apostrophe in a message is an apostrophe and never a second statement.</li>
<li data-code="Db.exec" data-href="providers/">Writes are serialized on this entity's own event loop, so concurrent callers queue rather than collide.</li>
</ul>

</div>

<div class="synqt-file" data-file="schema" markdown>
<span class="synqt-file__name"><strong>table</strong><span class="synqt-flow__path">db/relational/store/schema.sql</span></span>

```sql
CREATE TABLE IF NOT EXISTS messages (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    who     TEXT NOT NULL,
    body    TEXT NOT NULL,
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

<div class="synqt-tree">
<span class="synqt-tree__title">Project tree</span>
<ul class="synqt-tree__list">
<li class="synqt-tree__leaf"><span class="synqt-tree__file" data-file="config" tabindex="0" role="button" aria-label="Show synqt.yaml">synqt.yaml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="gate client" tabindex="0" role="button" aria-label="Show both client entities">client</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="gate" tabindex="0" role="button" aria-label="Show client/gate/Main.qml">gate</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="gate" tabindex="0" role="button" aria-label="Show client/gate/Main.qml">Main.qml</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">app</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="client" tabindex="0" role="button" aria-label="Show client/app/Main.qml">Main.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="web" tabindex="0" role="button" aria-label="Show the web edge entity">web</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="web" tabindex="0" role="button" aria-label="Show the edge's files">edge</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="web" tabindex="0" role="button" aria-label="Show web/edge/Edge.qml">Edge.qml</span></li>
<li class="synqt-tree__dir"><span class="synqt-tree__folder" data-file="database schema" tabindex="0" role="button" aria-label="Show the relational entities">db/relational</span></li>
<li class="synqt-tree__dir synqt-tree__dir--nested"><span class="synqt-tree__folder" data-file="database schema" tabindex="0" role="button" aria-label="Show the store entity's files">store</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="database schema" tabindex="0" role="button" aria-label="Show db/relational/store/Store.qml and db/relational/store/schema.sql">Store.qml</span></li>
<li class="synqt-tree__leaf synqt-tree__leaf--deep"><span class="synqt-tree__file" data-file="database schema" tabindex="0" role="button" aria-label="Show db/relational/store/Store.qml and db/relational/store/schema.sql">schema.sql</span></li>
</ul>
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
