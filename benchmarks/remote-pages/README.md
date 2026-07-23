<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# remote-pages: the first-load weight of edge-delivered pages

An edge-delivered page (a route declared `remote:` rather than `view:`) is never compiled
into the client bundle: the edge delivers it on demand the first time a visitor navigates
to it. A visitor who never reaches that page never downloads it. This harness measures how
many bytes that keeps off the first load.

It uses the [stall](../../examples/stall) storefront, whose two campaign pages
(`Campaign.qml` and `Members.qml`) are `remote:` routes. It builds the example twice through
the real `synqt build` path and weighs each client bundle with the shared
[measure-bundle.sh](../client/measure-bundle.sh) (raw, gzip, and Brotli):

- **remote** -- the example as written: the campaign pages are edge-delivered, so they are
  not in the first-load bundle.
- **compiled-in** -- the same example with those two routes rewritten to compiled-in `view:`
  routes, so qmlcachegen compiles the pages into the client module and they ship on first
  load.

The saving is the compiled-in weight minus the remote weight: the bytes a first-time visitor
does not download because the pages live on the edge. The two variants serve the same set of
first-load files (the pages compile into the `.wasm`, they are not separate served assets),
so the difference is in the compiled module bytes, not the file count.

## What the baseline records

[baseline.json](baseline.json) is a real run on the recorded host, Qt, and Emscripten. From
that run:

| variant      | raw bytes  | gzip bytes | Brotli bytes |
| ------------ | ---------- | ---------- | ------------ |
| remote       | 26092721   | 9581701    | 6746398      |
| compiled-in  | 26108258   | 9584939    | 6749377      |
| **saving**   | **15537**  | **3238**   | **2979**     |

## This is a measurement, not a claim

The saving above is what two small pages weigh in one small demo. It understates what a real
application saves, because the saving is entirely a function of how much of the app is rarely
visited: a storefront with dozens of seldom-reached campaign, help, legal, and admin pages
keeps all of them off every first load, and the saving grows with each one. A demo with two
tiny pages is the floor of the effect, not a representative figure. Read the number as "these
specific pages, on this build" and re-measure on your own application to learn its saving; do
not extrapolate this figure to a claim about SynQt in general.

## Reproduce

Needs the Qt for WebAssembly kit (`wasm_singlethread`) and the pinned Emscripten (4.0.7), so
it belongs on a workstation with that toolchain, not the build sandbox. From the repo root:

    benchmarks/remote-pages/run.sh --out benchmarks/results/remote-pages-$(hostname).json

The harness copies the example out of the tree for each variant and never touches the
committed `examples/stall/` source. It builds only the client entity, since the client bundle
is the only artifact weighed. Set `QT_WASM_ST` to override the kit location.
