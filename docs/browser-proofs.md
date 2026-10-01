<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Browser coverage

SynQt's client is WebAssembly in a real browser, and the link it depends on most
(QtRemoteObjects over QtWebSockets) is not officially supported by Qt. So "it works in
a browser" is proven by driving real browser engines. This page maps what each browser
harness covers and how to run it. It is written for people working on SynQt itself.
Building an application needs none of it.

## What is covered

| Harness | What it drives | Chromium | Firefox | WebKit | Runner |
|---------|----------------|----------|---------|--------|--------|
| Transport: property, signal, slot, and model over `ws` and `wss`, plus reconnect | QtRO over WebSockets, WebAssembly client against a native edge | covered | covered | opt in | [`tests/transport-spike/verify/verify.mjs`](https://github.com/Kidev/SynQt/blob/main/tests/transport-spike/verify/verify.mjs) |
| Transport, multi threaded: the same matrix on the threaded kit under COOP and COEP | threaded WebAssembly with SharedArrayBuffer | covered | covered | opt in | [`tests/transport-spike/verify/verify-mt.mjs`](https://github.com/Kidev/SynQt/blob/main/tests/transport-spike/verify/verify-mt.mjs) |
| Client counter: two tabs stay in sync | the full client runtime | covered | covered | opt in | [`tests/client/verify/verify.mjs`](https://github.com/Kidev/SynQt/blob/main/tests/client/verify/verify.mjs) |
| Generated app boot: a scaffolded app boots and connects over a live QtRO link | the `synqt dev` WebAssembly shell | covered | not targeted | not targeted | `synqt dev` |
| Qt Quick 3D Physics load: the scene links, the RHI comes up, the event loop runs | single threaded WebAssembly with PhysX | covered | not targeted | not targeted | [`tests/wasm-quick3dphysics/verify/verify-phys.mjs`](https://github.com/Kidev/SynQt/blob/main/tests/wasm-quick3dphysics/verify/verify-phys.mjs) |
| Qt Quick 3D Physics simulation: a box falls under gravity and rests on the plane | multi threaded WebAssembly with PhysX (`numThreads: 0`) | covered | not targeted | not targeted | [`tests/wasm-quick3dphysics/verify/run-phys-mt.sh`](https://github.com/Kidev/SynQt/blob/main/tests/wasm-quick3dphysics/verify/run-phys-mt.sh) |

`covered` means the harness drives that engine headless on every run. `opt in` means the
harness probes for the engine, drives it when it launches, and skips it with a message
when it is absent, so installing the engine is the only step needed to cover that column.
`not targeted` means the proof is about something other than engine differences.

The columns describe the harness rather than continuous integration. WebKit is `opt in` because a
developer machine may not have its runtime, and it is installed and driven on every run
of [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) below, on Ubuntu and on macOS.

WebKit is Safari's engine, and the closest stand in for Safari on a Linux or CI host.
It answers the engine question. The last mile (Safari's own TLS stack and WebGL
behavior) needs a run on macOS, which is what
[`verify-safari.mjs`](https://github.com/Kidev/SynQt/blob/main/tests/transport-spike/verify/verify-safari.mjs) is for.

## Running the harnesses

Every harness pins Qt 6.12.0 and Emscripten 5.0.5, builds what it needs through its own
`run-*.sh`, and reads the browser console for single line result markers.

The dated results further down were taken on Qt 6.11.1 with Emscripten 4.0.7, before the pin
moved, and each says so where it sits. Run one again on the current pin before quoting it as
a claim about that pin. A browser proof is a statement about one toolchain in one engine, and
a new Emscripten is the kind of change that can move it.

```sh
# Transport, on every engine present: ws, wss, and reconnect
tests/transport-spike/verify/run-spike.sh

# Transport on the multi threaded kit (SharedArrayBuffer under COOP and COEP)
tests/transport-spike/verify/run-mt.sh
MT_BROWSERS=chromium tests/transport-spike/verify/run-mt.sh   # narrow the engine set

# Transport in real Safari.app (macOS only, needs a GUI session)
sudo safaridriver --enable                     # once per machine
tests/transport-spike/verify/run-safari.sh
SAFARI_WSS=1 tests/transport-spike/verify/run-safari.sh   # after trusting the harness cert

# The client runtime: the native functional half, then two tab sync in every engine
tests/client/run-client.sh

# Qt Quick 3D Physics: single threaded load and boot, then the multi threaded fall
tests/wasm-quick3dphysics/verify/run-phys.sh
tests/wasm-quick3dphysics/verify/run-phys-mt.sh
```

To add WebKit, install its runtime dependencies first. Playwright's `install-deps`
targets Debian and Ubuntu. On another distribution, install the equivalent packages
(`libicu`, `libwoff2`, GStreamer, `libflite` and their dependencies) through its own
package manager.

```sh
sudo npx playwright install-deps
npx playwright install webkit
tests/transport-spike/verify/run-spike.sh    # the WebKit cases now run too
```

## In continuous integration

[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) runs the transport harness across Chromium, Firefox, and WebKit,
on dispatch and on a change to the spike, on Ubuntu and on macOS. This is the only harness
whose result depends on software outside this repository. The spike it drives is
stable, while the browser engines keep changing. The harness floats Playwright, so each run
resolves the engine builds that are current that day and prints their versions in its log,
which is what makes a green run comparable to the next one, and what makes an old green run
a statement about the engines of that day rather than today's. Dispatch it before leaning
on the result.

[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the proofs that need a WebAssembly kit no other workflow
installs: the multi threaded SharedArrayBuffer proof and the client runtime, each in every
engine, Qt Quick 3D Physics on both kits, and a real `synqt build` of the arena client
bundle. It is dispatched manually and on
changes to what it covers. Both workflows build a Qt module from source for the
WebAssembly kit, which ships no QtRemoteObjects, so both skip ordinary pushes.

## Known limits

- Safari.app is driven only by hand, on macOS. `run-safari.sh` covers the four QtRO
  paths and reconnect in Safari itself, and it passed on 2026-08-02 on macOS 15.7.8
  with Safari 26.6, on Qt 6.11.1 and Emscripten 4.0.7. Both workflows leave it out,
  because Safari has no headless mode, so it needs a logged in GUI session, and
  `safaridriver --enable` needs sudo once per machine. Its `wss` case is a further
  opt in (`SAFARI_WSS=1`), because Safari cannot
  be told to accept the harness's self signed certificate the way every other engine
  can, so that case only runs where the certificate has been trusted in the system
  keychain.
- Sustained load and interactive sessions (the multi player capstone load test and the
  client frame time benchmark) need a normal host with a display rather than a headless
  CI runner. [`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md) marks which harnesses those are.
- `worker-src 'self' blob:` is still emitted under cross origin isolation even though
  no engine SynQt targets needs it. Both questions it hedges against, whether an
  engine's loader uses `blob:` workers and whether it grants SharedArrayBuffer under
  those headers, are asked on every run of the multi threaded proof, in every engine
  that runs there. It serves the threaded bundle under a strict `worker-src 'self'`
  and prints each engine's violations. Chromium, Firefox, and WebKit have all
  answered no `blob:` and yes SharedArrayBuffer, WebKit on 2026-07-31 on macOS 15.7.8
  (WebKit 26.5), all on Qt 6.11.1 and Emscripten 4.0.7. The allowance stays as a margin
  for a future toolchain. See
  [Content-Security-Policy](csp.md).
