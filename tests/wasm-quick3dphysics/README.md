<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# wasm-quick3dphysics: is Qt Quick 3D Physics usable on WebAssembly?

The July 2026 spec audit recorded, as the reason the multiplayer tutorial was rewritten in
2D, that **Qt Quick 3D Physics does not work on WASM**. This fixture tests that directly against
the pinned Qt 6.11.1 kit, and the claim turns out to be false with the right configuration:
Quick3D Physics **builds, links, loads, boots, and steps** on WASM; the box falls under gravity
and rests on the plane, **headless, on both the single-threaded and the multi-threaded kit**.

The tutorial staying 2D is still a reasonable call (it keeps the arena simple and GPU-free), but
it is a choice, not a "Physics is impossible on WASM" necessity.

## The one line that makes it work

`PhysicsWorld.numThreads` (since Qt 6.7) defaults to **-1**, meaning *automatic*: Quick3D Physics
queries the host core count and steps PhysX on that many **worker threads**. That default is the
whole problem on WebAssembly:

- on the **single-threaded** kit the workers cannot be spawned at all, so the simulation never
  advances; the box sits at its release height (the original "does not work" symptom);
- on the **multi-threaded** kit PhysX creates worker pthreads and the browser main thread
  **deadlocks joining them**, freezing the page immediately after the scene loads (a live
  `Ready` scene, then no further frames; exactly the "freezes on loading" report).

Setting `numThreads: 0` steps the simulation **sequentially on the calling thread** (no worker
threads, no pthread dependency), which is the only configuration that runs in the browser, and it
runs on either kit. That single property is the fix; everything else in the scene is ordinary
Quick3D Physics.

## What is proven, and how

The scene is deliberately tiny: a `PhysicsWorld` (`numThreads: 0`), a `StaticRigidBody` plane at
y = -100, and a `DynamicRigidBody` box released at y = 200. `main.cpp` samples the box's height
from **C++** (`qWarning`, which reaches the browser console in the WASM runtime; QML
`console.log` does not, reliably, in a release build), prints the view's load status, and emits a
`PHYS exec` marker just before `app.exec()` so a stall can be located precisely.

Two harnesses, both headless Chromium, both a full GO on this checkout:

1. **Single-threaded kit** (`verify-phys.sh` -> `verify-phys.mjs`). Builds with
   `wasm_singlethread`, linking the `libqquick3dphysicsplugin.a` plugin and the
   `libQt6BundledPhysX.a` archive that ship in the kit (~34 MB `.wasm`). The `QQuickView` reaches
   status **Ready** with a live root object, the event loop starts, and the box falls:
   `simulation advances (box falls): PASS (lowestY=-50.00, ticks=45)`. The same script also runs
   the identical QML+C++ **natively** as the correctness reference
   (`PHYS done startY=200.00 minY=-53.68 finalY=-50.00`).
2. **Multi-threaded kit** (`verify-phys-mt.sh` -> `verify-phys-mt.mjs`). Builds with
   `wasm_multithread` and serves under the cross-origin-isolation headers `SharedArrayBuffer`
   needs (`COOP: same-origin` + `COEP: require-corp`, precisely what the SynQt web edge emits when
   `security.cross_origin_isolation` is on). The page is confirmed `crossOriginIsolated`, boots,
   and the box falls headless: `simulation advances (box falls): PASS (lowestY=-50.00, ticks=45)`.

The single-threaded result is the notable one: it is the **default** WASM kit and needs no
cross-origin isolation, so Quick3D Physics runs on a plain WASM page with only `numThreads: 0`.

## Run it

```sh
# single-threaded kit (default): load + boot + fall headless, plus the native reference
tests/wasm-quick3dphysics/verify/run-phys.sh

# multi-threaded kit: same, served cross-origin-isolated for SharedArrayBuffer
tests/wasm-quick3dphysics/verify/run-phys-mt.sh

# multi-threaded kit, interactive: keep serving so you can watch it in a real tab
tests/wasm-quick3dphysics/verify/run-phys-mt.sh --serve   # open the printed URL
```

Needs the pinned host kit (`/opt/Qt/6.11.1/gcc_64`), the WASM kits
(`/opt/Qt/6.11.1/wasm_singlethread` and, for the MT harness, `wasm_multithread`) with emsdk
4.0.7, and, for the native reference in `run-phys.sh`, a GL-capable display.
