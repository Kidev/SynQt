<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native desktop clients

A SynQt client is a Qt Quick application. WebAssembly is one way to package it, the
one the browser needs, but it is not the only one. The same `client/` QML can be
built as a native application for Windows, macOS, and Linux, connecting to the same
web edge, over the same secure link, under the same security model. One QML codebase
becomes a browser app and a native desktop app at once.

This follows from the architecture. The client is
already the most constrained entity in the system. The browser sandbox anchors its
shape, so it can only connect out, never listen, and holds no secret and no mesh
certificate. A native build lifts none of those constraints away. It keeps the
client to exactly the same trust position, so the QML you already wrote runs
unchanged. Desktop is a strict superset of the environment the client is written
against.

## What stays the same

A desktop client is still a client entity. Everything the
[programming model](programming-model.md) and the [runtime API](runtime-api.md)
describe applies without change:

- It is a connector. It reaches exactly one web edge over a WebSocket it opens
  itself, and never listens for mesh traffic. It holds no mesh certificate and is
  never a consumer of a service's connect point directly. It reaches services only
  through the edge, exactly as the browser does.
- It consumes its edge's connect point through `Server`, reacts to `<Owner>.on<Signal>`,
  and is gated by `scope` at acquisition. An under-scoped desktop user is refused
  the Replica just as a browser user is.
- It authenticates with a user session rather than a certificate. The two identity
  systems ([`Caller.isUser` versus `Caller.isEntity`](runtime-api.md#service-caller))
  are unchanged. A desktop user is still a user.
- The edge, the mesh, every service entity, and the whole authorization model are
  untouched. Adding a desktop target changes how the client is packaged and how it
  reaches the edge, and nothing on the server side.

Because the constraints are identical, there is no "desktop version" of the app to
maintain. There is one client, built for two or more targets.

## What differs on desktop

Five things differ, all on the client side, all handled by the framework.

### Terminating TLS

In the browser, the platform terminates `wss` for you. The WASM client sets no
`QSslConfiguration`, because `QSsl` does not work in the browser (see the [Qt for
WebAssembly notes](architecture.md#plane-b-transport-the-secure-pipes)). A native
client has no such limitation. It terminates its own TLS with `QSslSocket`,
verifying the edge's public certificate against the operating system trust store
(or a certificate you pin in config). It connects to the same public `wss`
endpoint the browser uses. Only who terminates the TLS differs.

### Knowing where the edge is

A browser client is served by the edge, so it learns the edge origin from the page
it loaded, since the runtime config is delivered with the bundle. Nobody serves a
desktop client, so it must be told the edge's public URL. You provide it in
[`build.desktop.edge_url`](#configuration), and it is compiled into the binary. An
app that has to reach more than one deployment is therefore more than one build,
because the edge a client trusts is not a preference a user should be able to retarget.

### Signing in

OAuth still runs entirely on the edge, and the desktop client never holds the client
secret, exactly as in the browser. What differs is only how the finished session
gets back to the app:

1. `Session.login()` takes an ephemeral port on `127.0.0.1`, then opens the user's
   system browser at the edge's `login` route, telling it that port as `return`.
2. The edge runs the normal server-side OAuth2 flow (PKCE, state, token exchange,
   ID token verification) and creates the session, all as documented in
   [authentication](authentication.md).
3. Instead of setting a cookie on its own origin, the edge redirects the system
   browser to that loopback URL (the native-app pattern of RFC 8252), handing back
   a **one-time claim code**. Not the session: a URL a browser was sent to is
   written into that browser's history, and on a shared machine the history
   outlives the sign-in.
4. The app exchanges the code for the session over its own verified connection to
   the edge, and presents the session on the `wss` handshake exactly as the browser
   presents its cookie. A native client terminates its own TLS, so it sets the
   header itself and needs no cookie jar.

   The session travels in the same header the browser uses, not in a WebSocket
   subprotocol. `security.session_transport: subprotocol` is refused, because Qt 6.11
   gives the edge no way to select the subprotocol it would have to echo; see
   [`session_transport`](project-layout-and-config.md#security-browser-hardening-and-connection-gating) for the measurement.

The whole flow is off unless a client entity lists the `desktop` target. You do not
turn it on. The edge reads `targets:` and decides. A project that ships no desktop
client has nothing that could receive a loopback answer, so its edge refuses to issue
one at all.

Four things make the round trip safe, and each one is a test in
[`m8-auth`](https://github.com/Kidev/SynQt/tree/main/tests/m8-auth):

- The `return` URL is an allowlist of exactly one shape: `http`, a loopback literal
  (`127.0.0.1` or `[::1]`, never the name `localhost`), a port, and nothing else, with
  no userinfo, no path, no query and no fragment. Anything else refuses the login outright,
  before the provider is contacted, because an open redirect here hands out sessions.
- The claim code lives for a minute, and the first attempt to spend it is the only
  one, right or wrong.
- Spending it needs a verifier the app generated and never sent anywhere except that
  one exchange. Its SHA-256 goes to the edge when the login starts, the way PKCE does
  it, so a code read out of a browser history buys nothing.
- The client refuses any arrival on its loopback port that does not carry the nonce it
  generated for the sign-in it started. Any local process can connect to that port, and
  a code from one of them would otherwise sign the visitor in as somebody else.

The browser is not left signed in either. The edge sets no session cookie at the end
of a desktop login, because the system browser is not the app.

`Session.logout()` calls the edge logout route with the credential this client holds,
drops it, and reconnects as an anonymous visitor. Unlike the browser, which has to
navigate to that route because the cookie is not the app's to clear, the native client
owns its credential and can end the session without leaving the window.

### Storing the session

The browser keeps the session in an httpOnly cookie it cannot read. The desktop app
keeps it in memory, for the life of the process. App code never sees a raw credential
either way. `Session` exposes state and identity, never the token.

By default that is the whole story. Signing in is once per launch, and closing the app
ends it. A project that wants the visitor to stay signed in asks for it:

```yaml
identity:
  desktop_session: device        # default: memory
  device:
    store:                       # where the edge keeps the device table
      name: sqlite
      file: .synqt/devices.db
    lifetime_days: 30            # absolute
    inactivity_days: 14          # since it was last used
    overlap_seconds: 120
    min_binding: user            # user | application (see below about hardware)
```

What is stored is a device credential rather than the session. It is an opaque pair the
edge issues, redeemable exactly once, at exactly one route, and what it buys is a fresh
session of the ordinary length. If the stored thing were the session id, "stay signed in
for a month" and "a stolen file is good for a month" would be one number, and the
pressure would always be to make it larger.

Three properties follow:

- **Every redemption rotates.** The generation just presented is retired and a new one
  takes its place, so a credential copied off a disk is good only until the machine it
  came from next starts up.
- **A retired generation coming back is an event.** Presented inside
  `overlap_seconds` it is the ordinary case, a client that lost the answer before it
  could store it, and it costs nothing. Presented after that window it means two copies
  exist, so the device and every session it opened are revoked and somebody signs in
  again. Theft stops being silent, which no file permission achieves.
- **Scope is re-derived at every redemption**, through the same
  [mapping hook](authentication.md) a login runs through. Somebody demoted yesterday
  does not carry yesterday's scope for the rest of the month.

Signing out deletes the credential on both sides, and the edge reads which one to
delete from what it recorded when it minted that session, not from anything the client
sends.

**A credential buys a session, not a connection.** The client spends it once per session
it gets accepted, and no more: if the session it bought cannot get a socket accepted, it
retries with that session rather than buying another one exactly like it. And what is
stored is deleted only when the edge refuses the credential itself. A rate limit (the
route allows 30 redemptions a minute per address, which is shared with every other
machine behind the same address), a network that is down, a proxy having a bad minute:
none of those is an answer about the credential, so the app waits and stays signed in.

#### Where it lives, per platform

| | store | binds to |
|---|---|---|
| macOS | Keychain Services, `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` | this application's code signature, on a signed build |
| Windows | Credential Manager, `CRED_PERSIST_LOCAL_MACHINE` | this OS user (DPAPI at rest) |
| Linux | the Secret Service (`org.freedesktop.secrets`) through libsecret | this OS user |

There is no file fallback, on any platform, in any build, including development. A
machine with no store persists nothing and its visitor signs in once per launch, which
is exactly what `desktop_session: memory` does everywhere. What makes this credential
safe to hand out at all is that a copy of it cannot be taken without taking the OS
store's protection with it.

Four limits apply:

- On Windows and Linux the boundary is the OS user rather than the application. Any process
  running as that user can read the item back. macOS is the only one of the three with
  a real per-application boundary, and only on a signed build, which is why
  `synqt build --deploy --sign` has a security consequence there and not only a
  Gatekeeper one.
- Nothing ever prompts. A locked keyring yields no secret rather than a password
  dialog, because this read happens before the first frame and a modal there is a hang
  on a headless or SSH session.
- A redeemed session carries the visitor's identity and scope, and no provider tokens.
  Tokens belong to the session the login created (see
  [session lifecycle](authentication.md#session-lifecycle)) and that session is gone by
  the next launch, so what a relaunch restores is who somebody is rather than a live
  authorization to call the provider's API on their behalf. Nothing in SynQt hands an
  entity those tokens today, so nothing breaks. A system that later needs them across a
  relaunch has to have the visitor sign in again, rather than keeping a
  30-day refresh token sitting on a disk.
- `min_binding` is a fleet policy control. The client reports the level of its own
  store, and a patched client can claim more than it has. Proving it would need key
  attestation, which SynQt does not do. It is the
  same kind of control as [route guards](programming-model.md) and a
  [`transport: local`](security.md) link.

Raising `min_binding` never breaks a platform. A client whose store cannot meet the
floor keeps the session it just signed in for, writes nothing, and behaves exactly as
it does under `desktop_session: memory`. `synqt check` warns at build time about which
machines that will be, so it is a choice rather than a surprise. Which machines those
are is a property of each machine rather than of the build, which is why the edge
settles it at enrolment.

There are two levels to choose between today. `hardware` is in the vocabulary and no
store reports it. Nothing here talks to a Secure Enclave or a TPM yet, so a project
that asked for it would turn persistence off on every platform at once rather than on
some of them. `synqt check` refuses that floor and says so, instead of leaving a
feature switched on and inert. The level keeps its name so that a store which does
reach it later has one to report.

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

`--client` selects which of the client entity's declared targets (see
[configuration](#configuration)) this build produces. It defaults to `wasm`, so a bare
`synqt build` produces the browser bundle even for a client that also declares `desktop`:

```cli
synqt build                        # the browser bundle (the default)
synqt build --client desktop       # the native desktop app only
synqt build --client all           # every target the entity declares
synqt build --client none          # the service entities and no client at all
```

The desktop client uses the host desktop Qt kit, the same kit the service
entities already build against, so it needs no extra toolchain beyond what a SynQt
project already resolves. It lands under `build/`, in the folder for the platform
it was built on:

```text
build/
  client/                 # the WebAssembly bundle (served by the edge)
  client-desktop/
    DEPLOY.txt            # the deployment step to run, for the platforms built here
    windows/              # the .exe, plus its Qt runtime once deployed
    macos/                # <client>.app, plus its Qt runtime once deployed
    linux/                # the binary, plus its Qt runtime once deployed
  edge/                   # the web edge, unchanged
  ...
```

A desktop build is native, so it is produced per host platform: build the Windows
app on Windows, the macOS app on macOS, the Linux app on Linux, or fan them out
across a CI matrix. Only the host's own folder is filled by a given run. The WASM
bundle, by contrast, builds anywhere.

The platform deployment step is yours to run. `synqt build` produces the binary
and its `THIRD-PARTY-LICENSES`, and writes a `DEPLOY.txt` naming the exact command to
run against the artifact that build produced (`windeployqt`, `macdeployqt`, or a
portable layout of binary plus Qt libraries on Linux). It is left out of the build
because it is where signing identities, entitlements, notarization, and installer
format live, none of which a framework can pick for you, and a half-deployed bundle
that looks finished is worse than one that says what is missing.

What the build does guarantee is that the step can be performed. On macOS the client
is built as a `.app` bundle, because `macdeployqt` operates on nothing else. A bare
executable would leave you rewriting the generated CMake before you could run the
command `DEPLOY.txt` tells you to run.

If you want the deployed tree out of the one command anyway, ask for it, and say what
you mean about signing, because `--deploy` will not guess:

```cli
synqt build --client desktop --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --deploy --unsigned
```

`--deploy` runs `macdeployqt` on macOS, `windeployqt` on Windows, and the portable layout
on Linux. `DEPLOY.txt` then names what is still outstanding, which is not the same thing
in the two cases.

Linux has no official Qt deployment tool, so SynQt does that one itself, along the same
lines `windeployqt` follows:

- `qmlimportscanner` (from your kit) reports which QML modules the client imports, and
  only those are copied. It resolves every Controls style rather than only the one you
  set, because the style is chosen at run time.
- The plugin directories the client can load follow from the Qt modules it links. A
  client that links Qt Gui gets `platforms/`, `imageformats/` and the rest, and one that
  links no Qt Sql gets no `sqldrivers/`.
- The transitive library closure of all of it is then copied to `lib/`. Transitive is the
  operative word, because a platform plugin and a QML module are opened at run time, so
  what they link appears nowhere in the binary's own dependency list.
- A `<client>.sh` launcher sets `LD_LIBRARY_PATH`, `QML_IMPORT_PATH` and `QT_PLUGIN_PATH`
  to point at the three. The binary also carries an `$ORIGIN/lib` rpath, so it works when
  run directly too.

Only system libraries are left to the host: the C runtime and the display server's client
libraries, exactly as any other native application on the platform expects. For a single
distributable file, wrap the tree with `linuxdeploy` or an AppImage recipe.

The second flag is mandatory because what an unsigned build costs is different on each
platform, and only one of the three answers is "it will not run":

| Platform | Unsigned binary | Signing is |
|----------|-----------------|------------|
| macOS | Gatekeeper refuses it anywhere but the machine that built it | **required** to distribute |
| Windows | runs, but SmartScreen warns every downloader about an unrecognised publisher | **strongly advised** |
| Linux | runs normally, since there is no binary code signing | **not applicable**, sign the package |

So `--deploy` alone is refused, and the refusal states which of those three applies to
the host you are on, and offers only the flags that host accepts. `--unsigned` is an
acknowledgement. On Linux it is the normal state, and on macOS it means local use only.

`--sign` takes a codesign identity on macOS (passed to `macdeployqt -codesign`, which
signs the frameworks and plugins inside the bundle before the bundle itself) and a
certificate subject name on Windows (`signtool /n`, timestamped so the signature
outlives the certificate). On Linux it is refused, with the reason. SynQt never
notarizes, because that needs your credentials and a network round trip, so `DEPLOY.txt`
gives you the `notarytool` command instead.

The bundle identifier defaults to a placeholder
(`com.example.<project>.<client>`) and is a CMake cache entry rather than a
`synqt.yaml` key, since it belongs with signing. Set it on the generated `host`
preset once. The cache keeps it for later builds:

```cli
cmake --preset host -DSYNQT_BUNDLE_ID=com.acme.gavel
```

Until the deploy step runs, the app finds Qt through the kit it was built against and
runs only on a machine that has that kit. Afterwards Qt travels with the app. This is
asserted end to end by [`tests/desktop-client/`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client),
which deploys a copy of the built app on whichever platform it runs on and checks that
the result carries its own Qt. On Linux it goes further and reads `/proc/<pid>/maps` of
the running client: every Qt library, QML module and plugin the process mapped has to
come from inside the deployed tree. That check exists because "it ran" proves nothing on
a developer machine, where a tree missing a library still starts, with the distribution's
own Qt quietly answering for it.

## Developing against a desktop client

`synqt dev --desktop` runs the client natively in a window with the same file
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
  - name: app
    type: client
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
  rejected, because a native client cannot discover its edge and must be told it.
- The client target is still a client. Every rule that protects the WASM client
  protects the desktop client too. It may not reference any `env:` secret, no
  service `server` file compiles into it, and it is never added as a direct
  consumer of a non-edge connect point.
- `edge_url` must be a `wss://` URL in a release build (plaintext `ws://` is
  allowed only against a dev edge on localhost).

## Licensing

The built desktop client links the desktop Qt, whose Qt Quick and Qt Quick
Controls modules are LGPLv3 under open-source Qt, unlike the Qt for WebAssembly
platform port, which is GPLv3 (and is why the browser client artifact is GPLv3).
The practical consequence is that a native desktop client can be distributed under
the LGPLv3 terms of the modules it links, where the same app compiled to WASM
carries the GPLv3 conveyance obligation. As always, a GPLv3-only add-on (Qt Quick
3D, Qt Quick 3D Physics, and the others listed in
[licensing](licensing.md)) makes any build that links it GPLv3, WASM or native
alike.

Do not reason about this by hand. `synqt build` generates a `THIRD-PARTY-LICENSES`
file per target from what that target links, so the desktop app and
the WASM bundle each carry an accurate, separately-derived license manifest. The
full analysis, including the LGPL relinking obligation for a statically linked
native app, is in [licensing](licensing.md).

## Out of scope

- Mobile targets (Android, iOS). The mechanism is the same (a native Qt Quick
  client connecting to the edge) and the constraints are the same, but the
  packaging, permissions, and store requirements are out of scope.
- In-app auto-update. Shipping updates to an installed desktop app (an updater,
  a release feed, code signing for updates) is left to your platform's tooling.
- Store submission specifics. The build produces the platform bundle. Notarizing,
  signing, and submitting it to a store are platform concerns outside SynQt.
