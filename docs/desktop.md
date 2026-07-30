# Native desktop clients

A SynQt client is a Qt Quick application. WebAssembly is one way to package it, the
one the browser needs, but it is not the only one. The same `client/` QML can be
built as a native application for Windows, macOS, and Linux, connecting to the same
web edge, over the same secure link, under the same security model. One QML codebase
becomes a browser app and a native desktop app at once.

This is a deliberate consequence of the architecture, not a bolt-on. The client is
already the most constrained entity in the system (the browser sandbox anchors its
shape: it can only connect out, never listen, and holds no secret and no mesh
certificate). A native build lifts none of those constraints away. It keeps the
client to exactly the same trust position, so the QML you already wrote runs
unchanged. Desktop is a strict superset of the environment the client is written
against.

## What stays the same

A desktop client is still a **client entity**. Everything the
[programming model](programming-model.md) and the [runtime API](runtime-api.md)
describe applies without change:

- It is a connector: it reaches exactly one web edge over a WebSocket it opens
  itself, and never listens for mesh traffic. It holds no mesh certificate and is
  never a consumer of a service's connect point directly; it reaches services only
  through the edge, exactly as the browser does.
- It consumes connect points through `Server.<name>`, reacts to `<Contract>.on<Signal>`,
  and is gated by `scope` at acquisition. An under-scoped desktop user is refused
  the Replica just as a browser user is.
- It authenticates with a **user session**, not a certificate. The two identity
  systems ([`Caller.isUser` versus `Caller.isEntity`](runtime-api.md#service-caller))
  are unchanged; a desktop user is still a user.
- The edge, the mesh, every service entity, and the whole authorization model are
  untouched. Adding a desktop target changes how the client is packaged and how it
  reaches the edge, nothing on the server side.

Because the constraints are identical, there is no "desktop version" of the app to
maintain. There is one client, built for two or more targets.

## What differs on desktop

Five things differ, all on the client side, all handled by the framework.

### Terminating TLS

In the browser, the platform terminates `wss` for you: the WASM client sets no
`QSslConfiguration`, because `QSsl` does not work in the browser (see the [Qt for
WebAssembly notes](architecture.md#plane-b-transport-the-secure-pipes)). A native
client has no such limitation: it terminates its own TLS with `QSslSocket`,
verifying the edge's public certificate against the operating system trust store
(or a certificate you pin in config). It connects to the **same public `wss`
endpoint** the browser uses; only who terminates the TLS differs.

### Knowing where the edge is

A browser client is served by the edge, so it learns the edge origin from the page
it loaded: the runtime config is delivered with the bundle. A desktop client is
not served by anyone; it must be told the edge's public URL. You provide it in
[`build.desktop.edge_url`](#configuration), and it is compiled into the binary. An
app that has to reach more than one deployment is therefore more than one build,
which is the honest shape: the edge a client trusts is not a preference a user
should be able to retarget.

### Signing in

OAuth still runs entirely on the edge, and the desktop client never holds the client
secret, exactly as in the browser. What differs is only how the finished session
gets back to the app:

1. `Session.login()` opens the user's **system browser** at the edge's `login`
   route.
2. The edge runs the normal server-side OAuth2 flow (PKCE, state, token exchange,
   ID token verification) and creates the session, all as documented in
   [authentication](authentication.md).
3. Instead of setting a cookie on its own origin, the edge redirects the system
   browser to a loopback URL the desktop app is briefly listening on
   (`http://127.0.0.1:<port>/...`, the native-app pattern of RFC 8252), handing
   back a one-time session token.
4. The app stores that token in the **OS secure store** (Keychain on macOS,
   Credential Manager on Windows, the Secret Service / libsecret on Linux) and
   presents it on the `wss` handshake, exactly as the browser presents its cookie.
   A native client terminates its own TLS, so it sets the header itself and needs no
   cookie jar. No secret and no long-lived token is ever written to plain disk.

   The session travels in the same header the browser uses, not in a WebSocket
   subprotocol. `security.session_transport: subprotocol` is refused, because Qt 6.11
   gives the edge no way to select the subprotocol it would have to echo; see
   [`session_transport`](project-layout-and-config.md#security-browser-hardening-and-connection-gating) for the measurement.

`Session.logout()` clears the stored token and calls the edge logout route, exactly
as in the browser.

### Storing the session

The browser keeps the session in an httpOnly cookie it cannot read; the desktop
app keeps it in the OS secure store. In both cases the app code never sees a raw
credential; `Session` exposes state and identity, never the token.

### Navigating without an address bar

A native window has no address bar and no History API, but
[`Router`](runtime-api.md#client-router) is the same object with the same members
here. The one class that knows a browser has a history keeps an equivalent stack in
memory on desktop, so `Router.go()`, `Router.replace()`, `Router.back()`, and
`Router.forward()` behave exactly as they do in a tab, and a Back button or a mouse
side button wired to `Router.back()` walks the same entries. Nothing in your QML
branches on the target.

Two consequences follow from there being no URL:

- There is no deep link to resolve at startup, so a native client always opens on
  `/`. `router.base` is a browser concern and is ignored.
- The [login resume](security.md#deep-links-and-the-login-resume) is held in memory
  rather than in `sessionStorage`, because the desktop client stays alive across the
  loopback redirect instead of navigating away and back. It is validated by the same
  rules and cleared the same way, so a visitor refused at `/admin` who then signs in
  is taken to `/admin` on desktop exactly as in the browser.

[Remote pages](remote-pages.md) work on desktop with no change. A `remote:` route is
delivered by the web edge over the same `wss` link, and a desktop build reaches the
same edge, so it fetches, caches, and renders a delivered page exactly as a browser
tab does, palette and page seed included. The edge enforces a page's `scope` before
delivery here too.

## Building for desktop

`synqt build` builds whichever targets the client entity declares (see
[configuration](#configuration)). Select or narrow them with `--client`:

```cli
synqt build --client wasm          # the browser bundle only
synqt build --client desktop       # the native desktop app only
synqt build --client all           # both (the default when both are declared)
```

The desktop client uses the **host desktop Qt kit**, the same kit the service
entities already build against, so it needs no extra toolchain beyond what a SynQt
project already resolves. It lands under `build/`, in the folder for the platform
it was built on:

```text
build/
  client/                 # the WebAssembly bundle (served by the edge)
  client-desktop/
    DEPLOY.txt            # the deployment step to run, for the platforms built here
    windows/              # the .exe, plus its Qt runtime once deployed
    macos/                # the binary, plus its Qt runtime once deployed
    linux/                # the binary, plus its Qt runtime once deployed
  web/                    # the web edge, unchanged
  ...
```

A desktop build is native, so it is produced per host platform: build the Windows
app on Windows, the macOS app on macOS, the Linux app on Linux, or fan them out
across a CI matrix. Only the host's own folder is filled by a given run. The WASM
bundle, by contrast, builds anywhere.

**The platform deployment step is yours to run.** `synqt build` produces the binary
and its `THIRD-PARTY-LICENSES`, and writes a `DEPLOY.txt` naming what to run
(`windeployqt`, `macdeployqt`, or a portable layout of binary plus Qt libraries on
Linux). It is left out of the build because it is where signing identities, entitlements,
notarization, and installer format live, none of which a framework can pick for you,
and a half-deployed bundle that looks finished is worse than one that says what is
missing.

## Developing against a desktop client

`synqt dev --desktop` runs the client **natively in a window** with the same file
watching and hot reload as the browser loop, against the same dev edge and the same
throwaway dev CA:

```cli
synqt dev                # the client in a browser (default)
synqt dev --desktop      # the client in a native window
```

The native loop is faster than the WebAssembly one, since a QML change reloads the
running window without an Emscripten link step, so it is a comfortable way to
iterate on UI even for an app you will ultimately ship to the browser. Behavior
that depends on a real browser (the exact wss/TLS termination, cookie transport)
should still be verified against `synqt dev` before release.

## Configuration

Declare the client's targets on the client entity, and give a `build.desktop`
section when desktop is one of them:

```yaml
entities:
  - name: client
    kind: client
    targets: [wasm, desktop]   # default [wasm]; add "desktop" for a native build

build:
  desktop:
    edge_url: wss://app.example.com/sync   # the public edge endpoint the app connects to
```

`edge_url` is the whole section. There is no platform list (a run builds for its own
host) and no application name (the client entity's name is it). Icons, bundle
identifiers, and signing belong to the deployment step above, which is
platform-specific and stays in the platform's own tooling.

Validation (in addition to the [general rules](project-layout-and-config.md#validation)):

- A client with `desktop` in `targets` but no `build.desktop.edge_url` is
  rejected: a native client cannot discover its edge and must be told it.
- The client target is still a client. Every rule that protects the WASM client
  protects the desktop client too: it may not reference any `env:` secret, no
  service `server` file compiles into it, and it is never added as a direct
  consumer of a non-edge connect point.
- `edge_url` must be a `wss://` URL in a release build (plaintext `ws://` is
  allowed only against a dev edge on localhost).

## Licensing

The built desktop client links the **desktop** Qt, whose Qt Quick and Qt Quick
Controls modules are LGPLv3 under open-source Qt, unlike the Qt for WebAssembly
platform port, which is GPLv3 (and is why the browser client artifact is GPLv3).
The practical consequence is that a native desktop client can be distributed under
the LGPLv3 terms of the modules it links, where the same app compiled to WASM
carries the GPLv3 conveyance obligation. As always, a GPLv3-only add-on (Qt Quick
3D, Qt Quick 3D Physics, and the others listed in
[licensing](licensing.md)) makes any build that links it GPLv3, WASM or native
alike.

Do not reason about this by hand. `synqt build` generates a `THIRD-PARTY-LICENSES`
file **per target** from what that target actually links, so the desktop app and
the WASM bundle each carry an accurate, separately-derived license manifest. The
full analysis, including the LGPL relinking obligation for a statically linked
native app, is in [licensing](licensing.md).

## Out of scope

- **Mobile targets (Android, iOS).** The mechanism is the same (a native Qt Quick
  client connecting to the edge) and the constraints are the same, but the
  packaging, permissions, and store requirements are out of scope.
- **In-app auto-update.** Shipping updates to an installed desktop app (an updater,
  a release feed, code signing for updates) is left to your platform's tooling.
- **Store submission specifics.** The build produces the platform bundle; notarizing,
  signing, and submitting it to a store are platform concerns outside SynQt.
