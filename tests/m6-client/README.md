<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M6: The client runtime (and the counter example)

`SynClient`, `ServerAccessor` (QML `Server`), `Session`, and `Router`, in the
`SynQtClient` library ([`src/client/`](../../src/client)). The same runtime links into
the WebAssembly client and a native desktop app from one QML, keeping the
connector-only trust position (no secret, no mesh certificate, reaches services only
through the edge). Members match [`docs/runtime-api.md`](../../docs/runtime-api.md).

## Status

| clause | status | evidence |
|--------|--------|----------|
| connection state transitions visible to QML (`Session.state`) | PASS | `tst_m6::stateTransitionsAreObservable` |
| two clients stay in sync (a shared edge-owned counter) | PASS | `tst_m6::counterSyncsBetweenClients` |
| a forced disconnect triggers reconnection (capped backoff) | PASS | `tst_m6::forcedDisconnectReconnects` |
| a route above the session scope redirects to the fallback | PASS | `tst_m6::routeGuardRedirectsAboveScope` |
| the same QML builds and runs as a native desktop app | PASS | `counter-client` (desktop) + `counter-edge` build; the native `tst_m6` runtime *is* the desktop runtime (own TLS + session) |
| slot dispatch to the owner's QML function (needed for the counter) | PASS | generator change; `tst_m6` increments the edge's `CounterSource.increment()` over the wss link |
| the counter runs end-to-end **in a browser** against the real edge | partial | the WASM client builds, loads, and reaches `state=connected` in Chromium under the strict CSP; the counter **value** does not yet surface via the dynamic replica in the browser (see below) |

`tst_m6` is the native functional test (6/6 passing) and exercises the runtime against
a real `WebEdge` over TLS; it is also the desktop runtime (native TLS termination +
session). The two native clients are the "two tabs" at the functional level.

## What the browser path proved (and the open issue)

Getting the WASM client to run under the edge's **strict CSP** (`script-src 'self'
'wasm-unsafe-eval'`, no `'unsafe-inline'`/`'unsafe-eval'`) surfaced four real findings,
all fixed here:

1. **Inline loader `<script>`**: the edge now hashes each inline script in the served
   page and adds `'sha256-...'` to `script-src` (`WebEdge::computeScriptHashes`).
2. **Inline `onload=` handler**: CSP hashes do not cover event handlers; the served
   shell registers `init` via an inline *script* (`addEventListener`) instead.
3. **`emscripten_run_script` uses `eval`**: the client reads `window.location` through
   the Embind `emscripten::val` bridge instead (no eval).
4. **The emscripten runtime emits `eval`/`new Function` by default**: the WASM client
   is built with `-sDYNAMIC_EXECUTION=0`.

After these, the WASM client loads and connects (`state=connected`) to the real edge in
a headless browser. **Open issue:** the counter *value* does not appear in the browser
(`Server.counter.value` stays undefined). It works natively (`tst_m6`, value crosses)
and M0 proved typed replicas replicate over a browser WebSocket, so the gap is specific
to a **dynamic replica acquired against a `QHttpServer`/upgrade edge in the browser**
not completing its QtRO handshake there. Under investigation; it does not affect the
native/desktop runtime or the mesh.

## How to run

```sh
tests/m6-client/run-m6.sh   # native test, desktop+WASM builds, browser end-to-end
```

## Layout

- `shared/Counter.syn`, `web/Counter.qml` (edge Source with `increment`/`decrement`),
  `client/Main.qml` + `client/main.cpp` (browser + desktop, one QML), `edge/main.cpp`
  (the counter web edge serving the bundle), `tst_m6.cpp` (native functional test),
  `app/` (desktop + WASM app builds), `verify/` (Playwright browser harness).
