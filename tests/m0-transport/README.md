<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The transport spike (QtRO over QtWebSockets)

The smallest end-to-end proof that QtRemoteObjects can ride QtWebSockets from a
WebAssembly client in a real browser. The Qt for WebAssembly docs call this transport
"not officially supported and may or may not work, or have missing functionality", and the
whole of SynQt is built on it, so it was proven here before any framework code. The spike
stays in the test suite as a permanent regression guard for that path.

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

Verified both headed on a real display (`DISPLAY=:0`) and headless, and on every
push by [browser-matrix.yml](../../.github/workflows/browser-matrix.yml).

One caveat, on the Qt side and not SynQt's. In Firefox on GitHub's hosted Ubuntu
runner, and nowhere else measured, a returning slot's reply arrives and decodes correctly
while `QRemoteObjectPendingCallWatcher::finished` never fires, because QtRO emits it over a
`Qt::QueuedConnection` whose posted events nothing drains there. The spike carries a 250 ms
poll that resolves the reply from `QRemoteObjectPendingCall`'s own state when the watcher
has not, and logs `(via poll fallback)` whenever it does. Grep a run log for that string to
see whether it is still happening. The full investigation, written up for upstream, with
the environment, the evidence trail and the ruled-out set, is in
[`FIREFOX-LINUX.md`](FIREFOX-LINUX.md).

The caveat has a reproduction and a fix, both off the runner.
[`verify/verify-pump.mjs`](verify/verify-pump.mjs) starves the single browser timeout Qt's
WASM event dispatcher arms to deliver posted events, which is enough to produce the same
failure in every engine on any machine. [`qt-patches/`](qt-patches/README.md) holds a
four-line change to `QEventDispatcherWasm::onTimer()` that makes it recoverable, with a
script that puts it on an installed kit and a before-and-after measurement. The poll
fallback stays until that lands in a Qt release, because CI builds against a stock Qt.

Safari and WebKit have two different proofs, because WebKit is Safari's engine and not
Safari itself.

`verify.mjs` drives Playwright's headless WebKit as the stand-in. The browser list
probes each engine for launchability and runs WebKit through the full four-direction and
reconnect matrix whenever its runtime is present. Where WebKit's system dependencies are
missing, since `npx playwright install-deps` needs root and targets Debian, the probe drops
it with a note and the gate still passes on Chromium and Firefox.

`verify-safari.mjs` and `run-safari.sh` drive real Safari.app through `safaridriver`,
which covers what Playwright's WebKit cannot: Apple's own TLS stack and networking. It
passed on 2026-08-02 on macOS 15.7.8 with Safari 26.6, on all four QtRO paths and reconnect
over `ws`. It is macOS-only and run by hand, never in CI. Safari has no headless mode, so
it needs a logged-in GUI session, and `safaridriver --enable` is a one-time sudo. Its `wss`
case is a further opt-in (`SAFARI_WSS=1`), because Safari is the one engine here that
cannot be told to accept a self-signed certificate. It has no `acceptInsecureCerts` and no
command-line switch, so that case runs only where the harness certificate has been trusted
in the system keychain.

Safari's WebDriver implements no logging endpoint, since the W3C spec has none and Apple
adds none, so the console the other engines are judged by does not exist there. The page
keeps its own log instead, through `console-tap.js`, which the harness injects ahead of the
Qt loader and `verify-safari.mjs` reads back over `execute/sync`. Both drivers reach their
verdict through the one `analyze()` in `harness.mjs`, so passing means the same thing in
each.

Multi-threaded WASM (`verify-mt.mjs` and `run-mt.sh`). The matrix above is the
single-threaded kit. The same client also builds with the `wasm_multithread` kit, which
needs `SharedArrayBuffer`, and the browser grants that only under cross-origin isolation:
`COOP: same-origin` and `COEP: require-corp`, exactly the headers the edge emits when
`security.cross_origin_isolation` is on. `run-mt.sh` builds the threaded client, serves it
with those headers, and asserts that the page is `crossOriginIsolated`, has
`SharedArrayBuffer`, boots the threaded runtime, and still passes all four QtRO paths. It
then serves the identical bundle without the headers and asserts that it is not isolated,
which proves the isolation comes from the headers. It runs in every engine Playwright can
launch, the same way the single-threaded matrix does.

The isolated page is served under the policy the edge emits, with one difference:
`worker-src 'self'` and no `blob:`. That measures on every run whether any engine needs the
`blob:` allowance the edge ships, and it reports each engine's `securitypolicyviolation`
events by directive. The result is reported and not enforced. An engine that turns out to
need `blob:` is a finding about that engine, and it would still work on the shipped policy.
As of 2026-07-31 all three engines have run it and none needed `blob:`: Chromium 149,
Firefox 151, and WebKit 26.5, the last of those on macOS 15.7.8, where it also confirmed
that WebKit grants `SharedArrayBuffer` under COOP and COEP and withholds it without them.
Serving the real policy is also why the client reads its `?url=` through Embind instead of
`emscripten_run_script_string` and links `-sDYNAMIC_EXECUTION=0`. `script-src
'wasm-unsafe-eval'` does not permit an eval, and without those two the spike would need a
policy no SynQt app uses.

## What it contains

- `shared/spike.rep`: a hand-written contract, since the `.syn` to `.rep` generator
  came later, exercising all four directions: `PROP counter` (READPUSH), `SIGNAL pinged`,
  `SLOT QString echo(...)` (a slot with a return value), and `MODEL rows(display)`.
- `shared/websocketiodevice.{h,cpp}`: the `QIODevice` adapter that carries QtRO over
  a `QWebSocket` (binary messages only), compiled into both the edge and the client.
- `edge/`: native listener. One `QRemoteObjectHost`
  (`setHostUrl(..., AllowExternalRegistration)`), a plaintext `QWebSocketServer`
  (`ws`, 8088) and a secure one (`wss`, 8089), each accepted socket wrapped and handed
  to the host with `addHostSideConnection`. No QtRO registry. `SpikeSource` drives the
  counter/signal/model on a 1s timer and answers `echo`.
- `client/`: a single-threaded WASM QML app. `M0Controller` wires the transport in C++
  (`QWebSocket` -> `WebSocketIoDevice` -> `node.addClientSideConnection` ->
  `setHeartbeatInterval` -> `acquire<SpikeSourceReplica>()`), sets no `QSslConfiguration`,
  since the browser terminates TLS, and reconnects by rebuilding the node, the socket and
  the adapter. It emits the `M0 ...` console sentinels the harness asserts on.
- `verify/`: the Playwright harness (`verify.mjs`) and the `run-m0.sh` orchestrator.

## How to run

```sh
tests/m0-transport/verify/run-m0.sh
```

That builds the edge on the host Qt kit and the client on the WASM kit, mints a
throwaway self-signed localhost certificate for the `wss` listener (a public-link TLS
server certificate and not a mesh CA, and nothing under `synqt/mesh/` is created),
installs Playwright, and runs the matrix and the reconnect case. Exit code 0 means it
passed. Set `M0_HEADLESS=1` to force headless, and `VERBOSE=1` to stream the sentinels.

Build directly without the harness:

```sh
# edge (native)
cmake -S tests/m0-transport -B build/m0-edge -G Ninja -DSYNQT_M0_ENTITY=edge \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.12.0/gcc_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m0-edge

# client (WASM single-threaded)
/opt/Qt/6.12.0/wasm_singlethread/bin/qt-cmake -S tests/m0-transport -B build/m0-client \
  -G Ninja -DSYNQT_M0_ENTITY=client -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client
```

## Findings

1. QtRemoteObjects is missing from the prebuilt Qt 6.12.0 WASM kits. The
   `wasm_singlethread` and `wasm_multithread` kits ship QtWebSockets and not
   QtRemoteObjects, with no CMake package, no `.a` and no QML plugin, and aqt does not
   offer it either. It has to be built from the pinned source
   (`/opt/Qt/6.12.0/Src/qtremoteobjects`) with each kit's `qt-cmake` and installed into
   the kit prefix. The kits' `qt-configure-module` is broken on Linux, with Windows
   backslashes in its paths, so use `qt-cmake` directly. The CLI performs this
   toolchain-provisioning step.
2. All four QtRO directions work over the WebSocket QIODevice in WASM, in both Chromium
   and Firefox, over both `ws` and `wss`. No path was missing functionality. Property
   push, signal delivery, a slot with a return value resolving on the client through
   `QRemoteObjectPendingCallWatcher`, and model replication (row count and incremental
   inserts) all behaved.
3. Reconnect works by tearing down and rebuilding the node, the socket and the adapter on
   `disconnected` or `errorOccurred`, with capped backoff. The replica re-initializes and
   fresh data resumes after the edge restarts. `SynClient` mirrors that shape. The spike
   does not reopen the same `QIODevice`, because a clean rebuild is the robust path and
   depends on no unspecified reuse semantics.
4. `wss` with a self-signed certificate requires the browser to accept that certificate
   (Playwright's `ignoreHTTPSErrors` and Chromium's `--ignore-certificate-errors`). That is
   expected for a throwaway dev certificate and is not a QtRO limitation. Production uses a
   real certificate.
5. The QtRO heartbeat carries liveness, and WebSocket ping and pong do not, because a
   WASM `QWebSocket` cannot send ping frames. The client node sets
   `setHeartbeatInterval(1000)`.

## Pinned versions used

The table above is the 2026-09-22 Linux run: Qt 6.12.0, Emscripten 5.0.5, Playwright
1.61.1 (Chromium build 1228, 149.0.7827.55; Firefox build 1532, 151.0), CMake 4.4.0,
Ninja 1.13.2. No case resolved through the poll fallback. The investigation in
[`FIREFOX-LINUX.md`](FIREFOX-LINUX.md) and the patch under [`qt-patches/`](qt-patches/README.md)
name the Qt 6.11.1 and Emscripten 4.0.7 toolchain they were measured on, and the patch
still applies to 6.12.0's source with no offset.

The 2026-08-02 macOS run: Chromium 149.0.7827.55, Firefox 151.0, WebKit 26.5 through
Playwright, and Safari 26.6 through `safaridriver`, on macOS 15.7.8.
