<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Desktop client: native compile + boot fixture

The SynQt client is one QML app with two packagings: the browser WASM bundle and a native
desktop executable built from the *same* QML and the same `SynClient` runtime (see
`docs/desktop.md`). `tests/appgen-native` proves the generated **service/edge** mains compile,
and it builds the client target as a side effect, but it never drove the `synqt build --client
desktop` tooling path, so the desktop-specific wiring (the host-preset build of the client, the
install into `build/client-desktop/linux/`, and the baked-in edge URL) was unproven. This fixture
closes that gap.

## What it does

Over the real three-entity **gavel** topology (client + web edge + persistence database, with
connect points, `per_session`, identity, and a provider), it:

1. marks the client `targets: [wasm, desktop]` and sets a distinctive `build.desktop.edge_url`;
2. runs the actual tooling (`presets.write` then `build.compile_incremental(client="desktop")`),
   which generates the client main/CMake, configures the `host` preset, compiles the client on
   the native kit, and installs it under `build/client-desktop/linux/`;
3. asserts the installed binary is a native ELF executable;
4. asserts the configured **edge URL is baked into the binary** (`SYNQT_EDGE_URL`; scanned in both
   ASCII and UTF-16 because `QStringLiteral` stores it as UTF-16); a desktop client has no
   serving origin to read its edge from, so this must come from `build.desktop.edge_url`;
5. boots the binary headless (`QT_QPA_PLATFORM=offscreen`, edge unreachable) and asserts it comes
   up and keeps running (loads the QML engine + `SynClient`, then blocks in `app.exec()`; killed
   by the timeout with code 124) rather than crashing or failing the QML load.

## What it caught

Writing a fixture that *runs* the tooling (not one that asserts generated strings) found that
`build.desktop.edge_url` was referenced only in a code comment and never passed to the compile:
the desktop client always linked the hardcoded CMake default `wss://127.0.0.1:8443/sync`,
silently ignoring the configured edge. `build._cmake_build` now forwards it as
`-DSYNQT_EDGE_URL`, and step 4 above is the regression guard. This is the same lesson as
`appgen-native`: string-level unit tests cannot see a value that never reaches the compiler.

## How to run

```sh
tests/desktop-client/run-desktop-client.sh
```

Needs the pinned host kit (`/opt/Qt/6.11.1/gcc_64`). Exit 0 means GO. No WASM kit or browser is
involved; the desktop client builds entirely on the host kit, which is the point of the target.
