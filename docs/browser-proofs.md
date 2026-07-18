<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Browser coverage

SynQt's client is WebAssembly in a real browser, and the link it depends on most
(QtRemoteObjects over QtWebSockets) is not officially supported by Qt. So "it works in
a browser" is proven by driving real browser engines, not asserted. This page is the map
of what each browser harness covers and how to run it. It is written for people working
on SynQt itself; building an application needs none of it.

## What is covered

| Harness | What it drives | Chromium | Firefox | WebKit | Runner |
|---------|----------------|----------|---------|--------|--------|
| Transport: property, signal, slot, and model over `ws` and `wss`, plus reconnect | QtRO over WebSockets, WebAssembly client against a native edge | covered | covered | opt in | `tests/m0-transport/verify/verify.mjs` |
| Transport, multi threaded: the same matrix on the threaded kit under COOP and COEP | threaded WebAssembly with SharedArrayBuffer | covered | covered | opt in | `tests/m0-transport/verify/verify-mt.mjs` |
| Client counter: two tabs stay in sync, reconnect, route guard | the full client runtime | covered | not targeted | not targeted | `tests/m6-client/verify/verify.mjs` |
| Generated app boot: a scaffolded app boots and connects over a live QtRO link | the `synqt dev` WebAssembly shell | covered | not targeted | not targeted | `synqt dev` |
| Qt Quick 3D Physics load: the scene links, the RHI comes up, the event loop runs | single threaded WebAssembly with PhysX | covered | not targeted | not targeted | `tests/wasm-quick3dphysics/verify/verify-phys.mjs` |
| Qt Quick 3D Physics simulation: a box falls under gravity and rests on the plane | multi threaded WebAssembly with PhysX (`numThreads: 0`) | covered | not targeted | not targeted | `tests/wasm-quick3dphysics/verify/run-phys-mt.sh` |

`covered` means the harness drives that engine headless on every run. `opt in` means the
harness probes for the engine, drives it when it launches, and skips it with a message
when it is absent, so installing the engine is the only step needed to cover that column.
`not targeted` means the proof is about something other than engine differences.

WebKit is Safari's engine, and the closest stand in for Safari on a Linux or CI host.
It answers the engine question; the last mile (Safari's own TLS stack and WebGL
behavior) needs a run on macOS.

## Running the harnesses

Every harness pins Qt 6.11.1 and Emscripten 4.0.7, builds what it needs through its own
`run-*.sh`, and reads the browser console for single line result markers.

```sh
# Transport, on every engine present: ws, wss, and reconnect
tests/m0-transport/verify/run-m0.sh

# Transport on the multi threaded kit (SharedArrayBuffer under COOP and COEP)
tests/m0-transport/verify/run-mt.sh
MT_BROWSERS=chromium tests/m0-transport/verify/run-mt.sh   # narrow the engine set

# The client runtime: two tab sync, reconnect, route guard
tests/m6-client/run-m6.sh

# Qt Quick 3D Physics: single threaded load and boot, then the multi threaded fall
tests/wasm-quick3dphysics/verify/run-phys.sh
tests/wasm-quick3dphysics/verify/run-phys-mt.sh
```

To add WebKit, install its runtime dependencies first. Playwright's `install-deps`
targets Debian and Ubuntu; on another distribution, install the equivalent packages
(`libicu`, `libwoff2`, GStreamer, `libflite` and their dependencies) through its own
package manager.

```sh
sudo npx playwright install-deps
npx playwright install webkit
tests/m0-transport/verify/run-m0.sh    # the WebKit cases now run too
```

## In continuous integration

`browser-matrix.yml` runs the transport harness across Chromium, Firefox, and WebKit.
`wasm-proofs.yml` runs the proofs that need a WebAssembly kit no other workflow
installs: the multi threaded SharedArrayBuffer proof, Qt Quick 3D Physics on both kits,
and a real `synqt build` of the arena client bundle. Both build a Qt module from source
for the WebAssembly kit, which ships no QtRemoteObjects, so both are dispatched manually
and on changes to what they cover rather than on every push.

## Known limits

- Safari itself runs only on macOS. WebKit covers the engine; a macOS pass covers
  Safari's own specifics.
- Sustained load and interactive sessions (the multi player capstone load test and the
  client frame time benchmark) need a normal host with a display, not a headless CI
  runner. `benchmarks/README.md` marks which harnesses those are.
- `worker-src 'self' blob:` is still emitted under cross origin isolation even though
  Chromium and Firefox provably do not need it, because two questions are WebKit's to
  answer: whether its loader uses `blob:` workers, and whether it grants
  SharedArrayBuffer under the same headers. See [Content-Security-Policy](csp.md).
