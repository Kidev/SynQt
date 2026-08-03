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

One caveat, and it is a Qt-side one rather than a SynQt one. In Firefox on GitHub's hosted
Ubuntu runner, and nowhere else measured, a returning slot's reply arrives and decodes
correctly but `QRemoteObjectPendingCallWatcher::finished` never fires, because QtRO emits it
over a `Qt::QueuedConnection` whose posted events are not drained there. The spike carries a
250 ms poll that resolves the reply from `QRemoteObjectPendingCall`'s own state when the
watcher has not, and logs `(via poll fallback)` whenever it does, so the workaround is never
silent: grep a run log for that string to see whether it is still happening. The full
investigation, written up for upstream, with the environment, the evidence trail and the
ruled-out set, is in [`FIREFOX-LINUX.md`](FIREFOX-LINUX.md).

That caveat now has a reproduction and a fix, both off the runner.
[`verify/verify-pump.mjs`](verify/verify-pump.mjs) starves the single browser timeout Qt's
WASM event dispatcher arms to deliver posted events, which is enough to produce the same
failure in every engine on any machine, and
[`qt-patches/`](qt-patches/README.md) holds a four-line change to
`QEventDispatcherWasm::onTimer()` that makes it recoverable, with a script that puts it on an
installed kit and a before-and-after measurement. The poll fallback stays until that lands in
a Qt release, because CI builds against a stock Qt.

Safari / WebKit. Two different proofs, because WebKit is Safari's engine but not Safari.

`verify.mjs` drives Playwright's headless WebKit as the in-env proxy: the browser list probes
each engine for launchability and runs WebKit through the full four-direction + reconnect
matrix whenever its runtime is present. Where WebKit's system dependencies are missing (`npx
playwright install-deps` needs root and targets Debian) the probe drops it with a note and the
gate still passes on Chromium + Firefox.

`verify-safari.mjs` / `run-safari.sh` drives real Safari.app through `safaridriver`, which
is the last mile Playwright's WebKit cannot cover: Apple's own TLS stack and networking. It
passed on 2026-08-02 on macOS 15.7.8 with Safari 26.6: all four QtRO paths and reconnect over
`ws`. It is macOS-only and run by hand, never in CI: Safari has no headless mode, so it needs a
logged-in GUI session, and `safaridriver --enable` is a one-time sudo. Its `wss` case is a
further opt-in (`SAFARI_WSS=1`), because Safari is the one engine here that cannot be told to
accept a self-signed certificate. It has no `acceptInsecureCerts`, no command-line switch: so that
case runs only where the harness cert has been trusted in the system keychain.

Safari's WebDriver implements no logging endpoint (the W3C spec has none and Apple adds none),
so the console the other engines are judged by does not exist there. The page therefore keeps
its own log: `console-tap.js`, injected by the harness ahead of the Qt loader, which
`verify-safari.mjs` reads back over `execute/sync`. Both drivers reach their verdict through
the one `analyze()` in `harness.mjs`, so "passing" means the same thing in each.

Multi-threaded WASM (`verify-mt.mjs` / `run-mt.sh`). The matrix above is the
single-threaded kit. The same client also builds with the `wasm_multithread` kit, which
needs `SharedArrayBuffer`, and the browser only grants that under cross-origin isolation
(`COOP: same-origin` + `COEP: require-corp`, exactly the headers the M5 edge emits when
`security.cross_origin_isolation` is on). `run-mt.sh` builds the threaded client, serves it
with those headers and asserts the page is `crossOriginIsolated`, has `SharedArrayBuffer`,
boots the threaded runtime, and still passes all four QtRO paths; then serves the identical
bundle without the headers and asserts it is *not* isolated, proving the headers are
load-bearing. Run in every engine Playwright can launch, the same way the single-threaded
matrix is.

The isolated page is served under the policy the edge actually emits, with one difference:
`worker-src 'self'` and no `blob:`. That turns the `blob:` allowance the edge ships into
something measured on every run rather than assumed, and each engine's
`securitypolicyviolation` events are reported by directive. It is reported, not enforced:
an engine that turns out to need `blob:` is a finding about that engine, and it would still
work on the shipped policy. As of 2026-07-31 all three engines have run it and none needed
`blob:`: Chromium 149, Firefox 151, and WebKit 26.5, the last of those on macOS 15.7.8, where
it also confirmed WebKit grants `SharedArrayBuffer` under COOP and COEP and withholds it
without them. Serving the real policy is also why the client reads its `?url=`
through Embind instead of `emscripten_run_script_string`, and links
`-sDYNAMIC_EXECUTION=0`: `script-src 'wasm-unsafe-eval'` does not permit an eval, so without
that the spike would need a policy no SynQt app uses.

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
not a mesh CA; nothing under `synqt/mesh/` is created), installs Playwright, and
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

1. QtRemoteObjects is missing from the prebuilt Qt 6.11.1 WASM kits. The
   `wasm_singlethread` / `wasm_multithread` kits ship QtWebSockets but not
   QtRemoteObjects (no CMake package, no `.a`, no QML plugin), and it is not available
   via aqt. It must be built from the pinned source (`/opt/Qt/6.11.1/Src/qtremoteobjects`)
   with each kit's `qt-cmake` and installed into the kit prefix. The kits'
   `qt-configure-module` is broken on Linux (Windows backslashes in its paths); use
   `qt-cmake` directly. This is a toolchain-provisioning step the M10 CLI must perform.
2. All four QtRO directions work over the WebSocket QIODevice in WASM, in both
   Chromium and Firefox, over both `ws` and `wss`. No missing functionality was
   observed on any of the four paths. Property push, signal delivery, a slot with a
   return value resolving on the client (`QRemoteObjectPendingCallWatcher`), and model
   replication (row count + incremental inserts) all behaved.
3. Reconnect works by tearing down and rebuilding the node, socket, and adapter on
   `disconnected`/`errorOccurred` with capped backoff; the replica re-initializes and
   fresh data resumes after the edge restarts. This is the shape the M6 `SynClient`
   will mirror. Same-`QIODevice` reopen was not relied upon; a clean rebuild is the
   robust path and avoids depending on unspecified reuse semantics.
4. `wss` with a self-signed cert requires the browser to accept the cert (Playwright
   `ignoreHTTPSErrors` + Chromium `--ignore-certificate-errors`). This is expected for a
   throwaway dev cert and is not a QtRO limitation; production uses a real cert.
5. The QtRO heartbeat, not WebSocket ping/pong, carries liveness (WASM QWebSocket
   cannot send ping frames). `setHeartbeatInterval(1000)` is set on the client node.

## Pinned versions used

Qt 6.11.1, Emscripten 4.0.7, Playwright 1.61.1 (Chromium 1228, Firefox 1532),
CMake 4.3.4, Ninja 1.13.2.

The 2026-08-02 macOS run: Chromium 149.0.7827.55, Firefox 151.0, WebKit 26.5 through
Playwright, and Safari 26.6 through `safaridriver`, on macOS 15.7.8.
