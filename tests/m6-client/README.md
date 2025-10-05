<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The client runtime (and the counter example)

`SynClient`, `ServerAccessor` (QML `Server`), `Session` and `Router`, in the
`SynQtClient` library ([`src/client/`](../../src/client)). The same runtime links into
the WebAssembly client and a native desktop app from one QML, and it keeps the
connector-only trust position in both: no secret, no mesh certificate, and it reaches
services only through the edge. Its members match
[`docs/runtime-api.md`](../../docs/runtime-api.md).

## Status

| clause | status | evidence |
|--------|--------|----------|
| connection state transitions visible to QML (`Session.state`) | PASS | `tst_m6::stateTransitionsAreObservable` |
| two clients stay in sync (a shared edge-owned counter) | PASS | `tst_m6::counterSyncsBetweenClients` |
| a forced disconnect triggers reconnection (capped backoff) | PASS | `tst_m6::forcedDisconnectReconnects` |
| a route above the session scope redirects to the fallback | PASS | `tst_m6::routeGuardRedirectsAboveScope` |
| the same QML builds and runs as a native desktop app | PASS | `counter-client` (desktop) + `counter-edge` build, and the native `tst_m6` runtime is the desktop runtime (own TLS + session) |
| slot dispatch to the owner's QML function (needed for the counter) | PASS | `tst_m6` increments the edge's `CounterSource.increment()` over the wss link |
| the counter runs end-to-end in a browser against the real edge, two tabs in sync | PASS | `verify/verify.mjs`, every installed engine |
| the browser back button reaches the router | PASS | `verify/verify.mjs`, the `<engine>` cases. The client pushes `/about`, the harness presses Back, `Router.path` returns to `/` |
| it still does with Qt's posted-event queue starved | PASS | `verify/verify.mjs`, the `<engine>-starved` cases (see below) |

`tst_m6` is the native functional test (6 of 6 passing) and exercises the runtime
against a real `WebEdge` over TLS. It is also the desktop runtime, with native TLS
termination and its own session. The two native clients are the two tabs at the
functional level.

## What the browser path proved

Running the WASM client under the edge's strict CSP (`script-src 'self'
'wasm-unsafe-eval'`, with no `'unsafe-inline'` and no `'unsafe-eval'`) needs four
things:

1. The inline loader `<script>` has to be allowed. The edge hashes each inline script in
   the served page and adds `'sha256-...'` to `script-src` (`WebEdge::computeScriptHashes`).
2. An inline `onload=` handler cannot be allowed, because CSP hashes do not cover event
   handlers. The served shell registers `init` from an inline script with
   `addEventListener` instead.
3. `emscripten_run_script` uses `eval`. The client reads `window.location` through
   the Embind `emscripten::val` bridge instead.
4. The emscripten runtime emits `eval`/`new Function` by default. The WASM client
   is built with `-sDYNAMIC_EXECUTION=0`.

With those four, the WASM client loads, connects, and the counter value crosses to both
tabs.

## The starved cases

Each engine runs twice. Qt for WebAssembly delivers posted events (the
`QEvent::MetaCall` behind a queued connection, the `QEvent::DeferredDelete` behind
`deleteLater`) through a single chain of two zero-delay browser callbacks, and it does not
re-arm that chain while one is pending. So one lost callback ends that delivery for the
life of the page, while timers, sockets and property updates carry on working. The
investigation is in
[`tests/m0-transport/FIREFOX-LINUX.md`](../m0-transport/FIREFOX-LINUX.md).

The `-starved` cases inject
[`pump-starve.js`](../m0-transport/verify/pump-starve.js), shared with the transport
spike instead of copied, which drops exactly that timeout and nothing else. They assert
that the client still connects, still navigates, and that the browser's back button still
reaches the router. That is the regression guard for the two things the client runtime had
to stop depending on. The popstate handler is called directly and never queued
(`src/client/browserhistory.cpp`), and deferred deletion goes through a timer instead of a
posted event (`SynQt::deleteSoon`).

Both halves matter. Restore the queued popstate hop and the plain cases still pass in
every engine while both `-starved` cases fail, which is the shape of the original defect.
Nothing errors, and only the one path that needs the queue is dead.

The starved cases cannot use a QML `Timer`, which is why a property-change handler
drives the navigation here. The animation framework backs `Timer`, and registering an
animation posts a queued call (`qabstractanimation.cpp`), so no QML timer or animation
starts once the queue is wedged.

## How to run

```sh
tests/m6-client/run-m6.sh   # native test, desktop+WASM builds, browser end-to-end
```

## Layout

- `shared/Counter.syn`, `web/Counter.qml` (edge Source with `increment`/`decrement`),
  `client/Main.qml` + `client/main.cpp` (browser + desktop, one QML), `edge/main.cpp`
  (the counter web edge serving the bundle), `tst_m6.cpp` (native functional test),
  `app/` (desktop + WASM app builds), `verify/` (Playwright browser harness).
