<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native desktop clients

A SynQt client is a Qt Quick application. WebAssembly is the packaging the browser needs,
but the same `client/` QML also builds as a native application for Windows, macOS and
Linux, connecting to the same web edge over the same secure link, under the same security
model. One QML codebase gives you a browser app and a desktop app.

This follows from the architecture. The client is already the most constrained entity in
the system: the browser sandbox means it can only connect out, never listen, and holds no
secret and no mesh certificate. A native build keeps every one of these constraints, so
the client keeps the same trust position and your QML runs unchanged. A desktop OS offers
everything the browser does, and more.

## What stays the same

A desktop client is still a client entity. Everything the
[programming model](programming-model.md) and the [runtime API](runtime-api.md)
describe applies without change:

- **It only connects out.** It reaches exactly one web edge over a WebSocket it opens,
  and never listens for mesh traffic. It holds no mesh certificate, never consumes a
  service's connect point directly, and reaches services only through the edge, like the
  browser.
- **It uses the same accessors.** It consumes its edge's connect point through `Server`,
  reacts to `<Owner>.on<Signal>`, and is gated by `scope` at acquisition: a desktop user
  below the scope is refused the Replica, like a browser user.
- **It authenticates with a user session,** not a certificate. The two identity systems
  ([`Caller.isUser` versus `Caller.isEntity`](runtime-api.md#service-caller)) are
  unchanged: a desktop user is still a user.
- **The server side does not change.** The edge, the mesh, every service entity and the
  authorization model stay the same. A desktop target changes only how the client is
  packaged and how it reaches the edge.

With identical constraints, there is no separate desktop version to maintain: one client
builds for two or more targets.

## What differs on desktop

Five things differ, all on the client side, and the framework handles all of them.

### Terminating TLS

In the browser, the platform terminates `wss`. The WASM client sets no
`QSslConfiguration`, because `QSsl` does not work in the browser (see the
[Qt for WebAssembly notes](architecture.md#plane-b-transport-the-secure-pipes)). A native
client terminates its own TLS with `QSslSocket`, verifying the edge's public certificate
against the OS trust store (or a certificate you pin in config). It connects to the same
public `wss` endpoint as the browser; only who terminates TLS differs.

### Knowing where the edge is

The edge serves a browser client, so the client learns the edge's origin from the page
it loaded, which carries the runtime config. Nobody serves a desktop client, so you give
it the edge's public URL in [`build.desktop.edge_url`](#configuration), which is compiled
into the binary. An app that must reach several deployments needs one build per
deployment, because users must not be able to point a client at a different edge.

### Signing in

OAuth still runs entirely on the edge, and the desktop client never holds the client
secret, as in the browser. Only the way the finished session returns to the app differs:

1. `Session.login()` takes an ephemeral port on `127.0.0.1`, then opens the system browser
   at the edge's `login` route, passing that port as `return`.
2. The edge runs the normal OAuth2 flow on the server (PKCE, state, token exchange, ID
   token verification) and creates the session, as described in
   [authentication](authentication.md).
3. Instead of setting a cookie on its own origin, the edge redirects the system browser to
   the loopback URL (the native app pattern of RFC 8252) with a one-time claim code, not
   the session. A URL sent to a browser lands in its history, and on a shared machine
   the history outlives the sign-in.
4. The app exchanges the code for the session over its own verified connection to the
   edge, and presents the session on the `wss` handshake as the browser presents its
   cookie. A native client terminates its own TLS, so it sets the header itself and needs
   no cookie jar.

   The session travels in the same header the browser uses, not in a WebSocket
   subprotocol. `security.session_transport: subprotocol` is refused, because Qt 6.12
   gives the edge no way to select the subprotocol it would have to echo. See
   [`session_transport`](project-layout-and-config.md#security-browser-hardening-and-connection-gating)
   for the measurement.

This flow is off unless a client entity lists the `desktop` target. You do not turn it on:
the edge reads `targets:` and decides. A project with no desktop client has nothing that
could receive a loopback answer, so its edge refuses to issue one.

Four rules make the round trip safe, each tested in
[`m8-auth`](https://github.com/Kidev/SynQt/tree/main/tests/m8-auth):

- **The `return` URL has exactly one allowed shape:** `http`, a loopback literal
  (`127.0.0.1` or `[::1]`, never the name `localhost`) and a port, with no userinfo,
  path, query or fragment. Anything else refuses the login before the provider is
  contacted, because an open redirect here would hand out sessions.
- **The claim code lives one minute,** and only the first attempt to spend it counts,
  right or wrong.
- **Spending it needs a verifier** the app generated and sent nowhere except that one
  exchange. Its SHA-256 goes to the edge when the login starts, as in PKCE, so a code read
  from a browser history is useless.
- **The client refuses any arrival on its loopback port** without the nonce it generated
  for this sign-in. Any local process can connect to that port, and a code from one of
  them would otherwise sign the visitor in as someone else.

The system browser is not left signed in: the edge sets no session cookie at the end of a
desktop login, because the browser is not the app.

`Session.logout()` calls the edge's logout route with the client's credential, drops it,
and reconnects as an anonymous visitor. The browser must navigate to that route because
it cannot clear its own cookie; the native client owns its credential and ends the
session without leaving the window.

### Storing the session

The browser keeps the session in an httpOnly cookie that page script cannot read. The
desktop app keeps it in memory for the life of the process. Either way, app code never
sees a raw credential: `Session` exposes state and identity, never the token.

By default that is all: the user signs in once per launch, and closing the app ends the
session. To keep the user signed in between launches, ask for it:

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

The app stores a device credential, not the session: an opaque pair the edge issues,
redeemable exactly once at exactly one route, in exchange for a new session of normal
length. If the app stored the session id, "stay signed in for a month" would also mean "a
stolen file works for a month", and there would always be pressure to make it longer.

Three properties follow:

- **Every redemption rotates.** The presented generation is retired and replaced, so a
  credential copied off a disk works only until the original machine next starts.
- **A retired generation that returns is an event.** Within `overlap_seconds`, it is the
  normal case of a client that lost the answer before storing it, and costs nothing.
  After that window, it means two copies exist, so the edge revokes the device and every
  session it opened, and the user signs in again. Theft stops being silent, which no file
  permission can achieve.
- **The scope is recomputed at every redemption,** through the same
  [mapping hook](authentication.md) as a login. Someone demoted yesterday does not keep
  yesterday's scope for the rest of the month.

Signing out deletes the credential on both sides. The edge picks which one to delete from
what it recorded when it created that session, not from anything the client sends. It
records the session's key (the handle a downstream entity receives), never the session
id: the device table is the one thing on the edge that outlives the process, and a copy
of it must not be a copy of every live session.

A credential buys a session, not a connection. The client spends it once per accepted
session. If the new session cannot get a socket accepted, the client retries with that
session instead of buying another. The client deletes the stored credential only when the
edge refuses the credential itself. A rate limit (30 redemptions a minute per visitor
address; behind a balancer, the address `public.trusted_proxies` resolves, otherwise one
budget for everyone sharing an address), a network outage or a misbehaving proxy says
nothing about the credential, so the app waits and stays signed in.

#### Where it lives, per platform

| | store | binds to |
|---|---|---|
| macOS | Keychain Services, `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` | this application's code signature, on a signed build |
| Windows | Credential Manager, `CRED_PERSIST_LOCAL_MACHINE` | this OS user (DPAPI at rest) |
| Linux | the Secret Service (`org.freedesktop.secrets`) through libsecret | this OS user |

There is no file fallback, on any platform or build, development included. A machine
without a store persists nothing, and its user signs in once per launch, as with
`desktop_session: memory`. The credential is safe to issue only because nobody can copy
it without defeating the OS store's protection.

Four limits apply:

- **On Windows and Linux, the boundary is the OS user,** not the application: any process
  of that user can read the item. Only macOS has a real per-application boundary, and
  only for a signed build, so `synqt build --deploy --sign` matters for security there,
  not just for Gatekeeper.
- **Nothing ever prompts.** A locked keyring returns no secret instead of showing a
  password dialog, because the read happens before the first frame, and a dialog there
  would hang a headless or SSH session.
- **A redeemed session carries the user's identity and scope, but no provider tokens.**
  Tokens belong to the session the login created (see
  [session lifecycle](authentication.md#session-lifecycle)), which is gone by the next
  launch. A relaunch restores who someone is, not a live authorization to call the
  provider's API for them. No SynQt entity reads those tokens, so nothing breaks. An app
  that calls the provider's API across a relaunch has the user sign in again, instead of
  keeping a 30 day refresh token on disk.
- **`min_binding` is a policy for your fleet, not a guarantee.** The client reports its
  own store's level, and a patched client can claim more than it has. Proving it would
  need key attestation, which SynQt does not do. It is the same kind of control as
  [route guards](programming-model.md) and a [`transport: local`](security.md) link.

Raising `min_binding` never breaks a platform. A client whose store cannot meet the floor
keeps the session it just signed in for, writes nothing, and behaves as under
`desktop_session: memory`. `synqt check` warns at build time which platforms that affects,
so you choose it knowingly. Whether a given machine qualifies depends on the machine, not
the build, so the edge decides at enrollment.

Two levels are usable. `hardware` exists in the vocabulary, but no store reports it:
nothing in SynQt talks to a Secure Enclave or a TPM, so asking for it would turn
persistence off on every platform. `synqt check` refuses that floor and says so, instead of
leaving a feature switched on that does nothing.

### Navigating without an address bar

A native window has no address bar and no History API, but
[`Router`](runtime-api.md#client-router) is the same object with the same members. On
desktop it keeps an equivalent history stack in memory, so `Router.go()`,
`Router.replace()`, `Router.back()` and `Router.forward()` behave as in a tab, and a Back
button or mouse side button wired to `Router.back()` walks the same entries. Your QML
never checks the target.

Without a URL:

- **There is no deep link at startup,** so a native client always opens on `/`.
  `router.base` applies only to browsers and is ignored.
- **The [login resume](security.md#deep-links-and-the-login-resume) lives in memory,** not
  in `sessionStorage`, because the desktop client stays alive across the loopback redirect
  instead of navigating away and back. The same rules validate and clear it, so a user
  refused at `/admin` who then signs in lands on `/admin`, as in the browser.

[Remote pages](remote-pages.md) work unchanged on desktop. The web edge delivers a
`remote:` route over the same `wss` link, and a desktop build reaches the same edge, so it
fetches, caches and renders delivered pages like a browser tab, palette and page seed
included. The edge enforces a page's `scope` before delivery here too.

## Building for desktop

`--client` selects which of the client entity's declared targets (see
[configuration](#configuration)) to build. It defaults to `wasm`, so a plain `synqt build`
produces the browser bundle even for a client that also declares `desktop`:

```cli
synqt build                        # the browser bundle (the default)
synqt build --client desktop       # the native desktop app only
synqt build --client all           # every target the entity declares
synqt build --client none          # the service entities and no client at all
```

The desktop client uses the host's desktop Qt kit, the same kit the service entities
build against, so it needs no extra toolchain. It lands under `build/`, in the folder for
the platform it was built on:

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

A desktop build is native, so each host platform builds its own: the Windows app on
Windows, the macOS app on macOS, the Linux app on Linux, or all three in a CI matrix. A run
fills only its host's folder. The WASM bundle builds anywhere.

You run the platform deployment step yourself. `synqt build` produces the binary and its
`THIRD-PARTY-LICENSES`, and writes a `DEPLOY.txt` naming the exact command to run on that
artifact (`windeployqt`, `macdeployqt`, or on Linux a portable layout of the binary plus
Qt libraries). The build leaves it out because this step involves signing identities,
entitlements, notarization and installer format, which a framework cannot choose for you,
and a half-deployed bundle that looks finished is worse than one that says what is
missing.

The build does guarantee that you can run the step. On macOS it builds the client as a
`.app` bundle, the only input `macdeployqt` accepts; with a bare executable you would have
to rewrite the generated CMake first.

To get the deployed tree from one command anyway, ask for it and state your signing
intent, because `--deploy` will not guess:

```cli
synqt build --client desktop --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --deploy --unsigned
```

`--deploy` runs `macdeployqt` on macOS, `windeployqt` on Windows, and the portable layout
on Linux. `DEPLOY.txt` then names what is left to do, which differs with and without
`--deploy`.

Linux has no official Qt deployment tool, so SynQt does the job itself, the way
`windeployqt` does:

- `qmlimportscanner` (from your kit) reports which QML modules the client imports, and
  only those are copied. It includes every Controls style, not only the one you set,
  because the style is chosen at run time.
- The Qt modules the client links decide which plugin directories it can load: a client
  linking Qt Gui gets `platforms/`, `imageformats/` and the rest, and one linking no Qt Sql
  gets no `sqldrivers/`.
- All the libraries these need, directly or indirectly, are copied to `lib/`. The indirect
  ones matter: platform plugins and QML modules are opened at run time, so what they link
  never appears in the binary's own dependency list.
- A `<client>.sh` launcher sets `LD_LIBRARY_PATH`, `QML_IMPORT_PATH` and `QT_PLUGIN_PATH` to
  these directories. The binary also has an `$ORIGIN/lib` rpath, so it runs directly too.

Only system libraries come from the host: the C runtime and the display server's client
libraries, as for any native application. For a single distributable file, wrap the tree
with `linuxdeploy` or an AppImage recipe.

The signing flag is mandatory because an unsigned build costs something different on each
platform, and only one refuses to run it:

| Platform | Unsigned binary | Signing is |
|----------|-----------------|------------|
| macOS | Gatekeeper refuses it anywhere but the machine that built it | **required** to distribute |
| Windows | runs, but SmartScreen warns every downloader about an unrecognised publisher | **strongly advised** |
| Linux | runs normally, since there is no binary code signing | **not applicable**, sign the package |

So `--deploy` alone is refused. The message says which case applies to your host and
offers only the flags it accepts. `--unsigned` is an acknowledgement: normal on Linux, and
on macOS it means local use only.

`--sign` takes a codesign identity on macOS (passed to `macdeployqt -codesign`, which signs
the frameworks and plugins inside the bundle before the bundle itself), and a certificate
subject name on Windows (`signtool /n`, timestamped so the signature outlives the
certificate). On Linux it is refused, with the reason. SynQt never notarizes, since that
needs your credentials and a network round trip; `DEPLOY.txt` gives you the `notarytool`
command instead.

The bundle identifier defaults to a placeholder (`com.example.<project>.<client>`). It is
a CMake cache entry, not a `synqt.yaml` key, because it belongs with signing. Set it once
on the generated `host` preset, and the cache keeps it:

```cli
cmake --preset host -DSYNQT_BUNDLE_ID=com.acme.gavel
```

Before the deploy step, the app finds Qt through the kit it was built against and runs
only on a machine with that kit. After it, Qt travels with the app.
[`tests/desktop-client/`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client)
checks this end to end: it deploys a copy of the built app on its platform and checks that
the result carries its own Qt. On Linux it also reads the running client's
`/proc/<pid>/maps`: every Qt library, QML module and plugin the process mapped must come
from inside the deployed tree. Just running proves nothing on a developer machine, where a
tree missing a library still starts, silently using the distribution's Qt.

## Developing against a desktop client

`synqt dev --desktop` runs the client natively in a window, with the same file watching
and hot reload as the browser loop, against the same development edge and throwaway
development CA:

```cli
synqt dev                # the client in a browser (default)
synqt dev --desktop      # the client in a native window
```

The native loop is faster than the WebAssembly one, because a QML change reloads the
running window without an Emscripten link step. It is a comfortable way to work on UI
even for an app you will ship to the browser. Still check anything that depends on a real
browser (wss and TLS termination, cookie transport) with `synqt dev` before release.

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
host) and no application name (it is the client entity's name). Icons, bundle identifiers
and signing belong to the platform deployment step above, in each platform's own
tooling.

Validation (in addition to the [general rules](project-layout-and-config.md#validation)):

- A client with `desktop` in `targets` but no `build.desktop.edge_url` is rejected: a
  native client cannot discover its edge.
- The desktop client is still a client, so every rule protecting the WASM client applies:
  it may not reference an `env:` secret, no service `server` file compiles into it, and it
  may never directly consume a connect point that a web edge does not own.
- `edge_url` must be a `wss://` URL in a release build (plaintext `ws://` is allowed only
  against a development edge on localhost).

## Licensing

The desktop client links the desktop Qt, whose Qt Quick and Qt Quick Controls modules are
LGPLv3 under open source Qt. The Qt for WebAssembly platform port is GPLv3, which is why
the browser client is GPLv3. So a native desktop client can be distributed under the
LGPLv3 terms of the modules it links, while the same app compiled to WASM carries the
GPLv3 obligation. Any GPLv3-only add-on (Qt Quick 3D, Qt Quick 3D Physics, and the others
listed in [licensing](licensing.md)) makes any build that links it GPLv3, WASM or native.

Do not work this out by hand. `synqt build` generates a `THIRD-PARTY-LICENSES` file per
target from what that target links, so the desktop app and the WASM bundle each carry an
accurate license list. [Licensing](licensing.md) has the full analysis, including the
LGPL relinking obligation for a statically linked native app.

## Out of scope

- **Mobile targets (Android, iOS).** The mechanism and constraints would be the same (a
  native Qt Quick client connecting to the edge), but packaging, permissions and store
  requirements are out of scope.
- **In-app auto-update.** Updating an installed desktop app (an updater, a release feed,
  signed updates) is left to your platform's tooling.
- **Store submission.** The build produces the platform bundle; notarizing, signing and
  submitting it to a store happen outside SynQt.
