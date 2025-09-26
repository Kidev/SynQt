<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Desktop client: native compile + boot fixture

The SynQt client is one QML app with two packagings: the browser WASM bundle and a native
desktop executable built from the *same* QML and the same `SynClient` runtime (see
[desktop clients](../../docs/desktop.md)). `tests/appgen-native` proves the generated service/edge mains compile,
and it builds the client target as a side effect, but it never drove the `synqt build --client
desktop` tooling path, so the desktop-specific wiring (the host-preset build of the client, the
install into `build/client-desktop/<platform>/`, and the baked-in edge URL) was unproven. This fixture
closes that gap.

## What it does

Over the real three-entity gavel topology (client + web edge + persistence database, with
connect points, `per_session`, identity, and a provider), it:

1. marks the client `targets: [wasm, desktop]` and sets a distinctive `build.desktop.edge_url`;
2. runs the actual tooling (`presets.write` then `build.compile_incremental(client="desktop")`),
   which generates the client main/CMake, configures the `host` preset, compiles the client on
   the native kit, and installs it under `build/client-desktop/<platform>/`;
3. asserts the installed binary is a native executable for the host (ELF, Mach-O or PE, whichever this platform links);
4. asserts the configured edge URL is baked into the binary (`SYNQT_EDGE_URL`; scanned in both
   ASCII and UTF-16 because `QStringLiteral` stores it as UTF-16); a desktop client has no
   serving origin to read its edge from, so this must come from `build.desktop.edge_url`;
5. deploys a *copy* of the built artifact through the tooling's own `deploy` module, on whichever
   platform it is running on, and asserts the result carries its own Qt;
6. boots the binary headless (`QT_QPA_PLATFORM=offscreen`, edge unreachable) and asserts it comes
   up and keeps running (loads the QML engine + `SynClient`, then blocks in `app.exec()`; killed
   by the timeout with code 124) rather than crashing or failing the QML load.

Step 5 is per platform, because what "carries its own Qt" looks like is:

| Platform | Asserted |
|----------|----------|
| macOS | an `.app` bundle with `Info.plist`, `QtCore.framework` inside it after `macdeployqt`, and a bundle-relative rpath to find it by |
| Windows | the Qt DLLs and `platforms/qwindows.dll` beside the exe after `windeployqt` |
| Linux | the libraries only a plugin or a QML module needs are present, nothing unimported was shipped, and the running client maps every Qt library from inside the tree (read from `/proc/<pid>/maps`) |

## What the steps guard

Step 5 guards the Linux deploy's dependency closure. A deploy that copies only the client
binary's own dependencies ships a tree that cannot start. A platform plugin and a QML module
are opened at run time, so nothing they link appears in the binary's list, and the tree
goes out without `libQt6XcbQpa.so.6` (the X11 platform plugin, so no window at all) or
`libQt6QuickControls2Impl` (so `import QtQuick.Controls` fails), among others. It looks
fine on any machine whose distribution packages Qt, because the loader answers from
`/usr/lib` instead. On a machine without Qt it exits 255 at the first QML import. A unit
test that mocks the dependency reader cannot see this, and `macdeployqt` computes the
closure itself, so the step runs on every platform. The deploy scopes with
`qmlimportscanner` and walks the transitive closure, which also keeps the tree at 294 MB
rather than the 931 MB that copying the kit's entire `qml/` and `plugins/` costs.

Step 4 guards `build.desktop.edge_url` reaching the compiler. A build that leaves it out
links the hardcoded CMake default `wss://127.0.0.1:8443/sync` and silently ignores the
configured edge. `build._cmake_build` forwards it as `-DSYNQT_EDGE_URL`. This is the same
lesson as `appgen-native`. String-level unit tests cannot see a value that never reaches
the compiler.

## How to run

```sh
tests/desktop-client/run-desktop-client.sh
```

Needs the pinned host kit (`/opt/Qt/6.11.1/gcc_64`). Exit 0 means GO. No WASM kit or browser is
involved; the desktop client builds entirely on the host kit, which is the point of the target.
