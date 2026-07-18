<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M0: Transport spike (QtRO over QtWebSockets)

This is the SynQt M0 go/no-go gate: the smallest end-to-end proof that
QtRemoteObjects can ride QtWebSockets from a WebAssembly client in a real browser.
The Qt for WebAssembly docs call this transport "not officially supported and may or
may not work, or have missing functionality," and the whole of SynQt is built on it,
so it is proven here before any framework code. This spike is kept in the test suite
as a permanent regression guard for that path.

## Verdict

**GO.** All four QtRO directions plus reconnect pass in Chromium and Firefox over
both plaintext `ws` and real `wss`.

| case | prop push | signal | slot (+return) | model | result |
|------|-----------|--------|----------------|-------|--------|
| chromium / ws  | pass | pass | pass | pass | PASS |
| chromium / wss | pass | pass | pass | pass | PASS |
| firefox / ws   | pass | pass | pass | pass | PASS |
| firefox / wss  | pass | pass | pass | pass | PASS |
| chromium / reconnect (edge restart) | n/a | n/a | n/a | n/a | PASS |
| firefox / reconnect (edge restart)  | n/a | n/a | n/a | n/a | PASS |

Verified both headed on a real display (`DISPLAY=:0`) and headless; the archived
headless run is `build/m0-verify.log`.

**Safari / WebKit.** Safari itself is macOS-only, but Safari's *engine* is WebKit, and
`verify.mjs` now drives Playwright's headless WebKit as the in-env proxy for it: the browser
list probes each engine for launchability and runs WebKit through the full four-direction +
reconnect matrix whenever its runtime is present. On this Arch Linux build host WebKit's system
dependencies are not installed (`npx playwright install-deps` needs root and targets Debian), so
the probe drops WebKit with a note and the gate still passes on Chromium + Firefox; WebKit's run
is a `sudo npx playwright install-deps` away on a Debian/Ubuntu CI runner. M0 is not declared
*fully* passed until the matrix runs against WebKit (any host) and, ideally, real Safari on
macOS; Chromium and Firefox are green. See `docs/browser-proofs.md` for the whole matrix.

**Multi-threaded WASM (`verify-mt.mjs` / `run-mt.sh`).** The matrix above is the
single-threaded kit. The same client also builds with the `wasm_multithread` kit, which
needs `SharedArrayBuffer`, and the browser only grants that under cross-origin isolation
(`COOP: same-origin` + `COEP: require-corp`, exactly the headers the M5 edge emits when
`security.cross_origin_isolation` is on). `run-mt.sh` builds the threaded client, serves it
**with** those headers and asserts the page is `crossOriginIsolated`, has `SharedArrayBuffer`,
boots the threaded runtime, and still passes all four QtRO paths; then serves the identical
bundle **without** the headers and asserts it is *not* isolated, proving the headers are
load-bearing. Verified headless on Chromium (`chromium-1228`, emsdk 4.0.7). This closes the
multi-threaded-WASM runtime check the single-threaded matrix does not cover.

## What it contains

- `shared/spike.rep`: hand-written contract (the `.syn` -> `.rep` generator is M1)
  exercising all four directions: `PROP counter` (READPUSH), `SIGNAL pinged`,
  `SLOT QString echo(...)` (a slot with a return value), `MODEL rows(display)`.
- `shared/websocketiodevice.{h,cpp}`: the `QIODevice` adapter that carries QtRO over
  a `QWebSocket` (binary messages only), compiled into both the edge and the client.
- `edge/`: native listener. One `QRemoteObjectHost`
  (`setHostUrl(..., AllowExternalRegistration)`), a plaintext `QWebSocketServer`
  (`ws`, 8088) and a secure one (`wss`, 8089), each accepted socket wrapped and handed
  to the host with `addHostSideConnection`. No QtRO registry. `SpikeSource` drives the
  counter/signal/model on a 1s timer and answers `echo`.
- `client/`: WASM single-threaded QML app. `M0Controller` wires the transport in C++
  (`QWebSocket` -> `WebSocketIoDevice` -> `node.addClientSideConnection` ->
  `setHeartbeatInterval` -> `acquire<SpikeSourceReplica>()`), sets no `QSslConfiguration`
  (the browser terminates TLS), and reconnects by rebuilding node+socket+adapter. It
  emits `M0 ...` console sentinels the harness asserts on.
- `verify/`: Playwright harness (`verify.mjs`) + `run-m0.sh` orchestrator.

## How to run

```sh
tests/m0-transport/verify/run-m0.sh
```

This builds the edge (host Qt kit) and client (WASM kit), mints a throwaway
self-signed localhost cert for the `wss` listener (a public-link TLS server cert;
**not** a mesh CA; nothing under `synqt/mesh/` is created), installs Playwright, and
runs the matrix + reconnect. Exit code 0 means GO. Set `M0_HEADLESS=1` to force
headless, `VERBOSE=1` to stream the sentinels.

Build directly without the harness:

```sh
# edge (native)
cmake -S tests/m0-transport -B build/m0-edge -G Ninja -DSYNQT_M0_ENTITY=edge \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.11.1/gcc_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m0-edge

# client (WASM single-threaded)
/opt/Qt/6.11.1/wasm_singlethread/bin/qt-cmake -S tests/m0-transport -B build/m0-client \
  -G Ninja -DSYNQT_M0_ENTITY=client -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client
```

## Findings (recorded per the M0 gate)

1. **QtRemoteObjects is missing from the prebuilt Qt 6.11.1 WASM kits.** The
   `wasm_singlethread` / `wasm_multithread` kits ship QtWebSockets but not
   QtRemoteObjects (no CMake package, no `.a`, no QML plugin), and it is not available
   via aqt. It must be built from the pinned source (`/opt/Qt/6.11.1/Src/qtremoteobjects`)
   with each kit's `qt-cmake` and installed into the kit prefix. The kits'
   `qt-configure-module` is broken on Linux (Windows backslashes in its paths); use
   `qt-cmake` directly. This is a toolchain-provisioning step the M10 CLI must perform.
2. **All four QtRO directions work over the WebSocket QIODevice in WASM**, in both
   Chromium and Firefox, over both `ws` and `wss`. No missing functionality was
   observed on any of the four paths. Property push, signal delivery, a slot with a
   return value resolving on the client (`QRemoteObjectPendingCallWatcher`), and model
   replication (row count + incremental inserts) all behaved.
3. **Reconnect works** by tearing down and rebuilding the node, socket, and adapter on
   `disconnected`/`errorOccurred` with capped backoff; the replica re-initializes and
   fresh data resumes after the edge restarts. This is the shape the M6 `SynClient`
   will mirror. Same-`QIODevice` reopen was not relied upon; a clean rebuild is the
   robust path and avoids depending on unspecified reuse semantics.
4. **`wss` with a self-signed cert requires the browser to accept the cert** (Playwright
   `ignoreHTTPSErrors` + Chromium `--ignore-certificate-errors`). This is expected for a
   throwaway dev cert and is not a QtRO limitation; production uses a real cert.
5. **The QtRO heartbeat, not WebSocket ping/pong, carries liveness** (WASM QWebSocket
   cannot send ping frames). `setHeartbeatInterval(1000)` is set on the client node.

## Pinned versions used

Qt 6.11.1, Emscripten 4.0.7, Playwright 1.61.1 (Chromium 1228, Firefox 1532),
CMake 4.3.4, Ninja 1.13.2.
