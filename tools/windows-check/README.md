<!--
SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
SPDX-License-Identifier: Apache-2.0
-->

# Windows cross-compile gate

A local pre-push check that compiles the SynQt native targets for the Windows MSVC ABI
from this Linux workstation, so the "does the Windows code path even build" class of
failure is caught here instead of on a CI round trip. It uses `clang-cl` and `lld-link`
against the Microsoft CRT and Windows SDK that [`xwin`](https://github.com/Jake-Shadle/xwin)
fetches, linking the Windows Qt kit while running the code generators (`moc`, `rcc`,
`repc`) from the Linux host Qt kit.

## What it does and does not catch

It **does** catch: `Q_OS_WIN` blocks that do not compile, wrong or missing Windows
includes and types, MSVC STL and SDK header incompatibilities, the named-pipe and ACL
API usage compiling and linking at all, ABI-level mistakes. A target that builds here
builds under the CI's `cl.exe`.

It **does not** catch: the exact `cl.exe /W4 /WX` warning verdict (clang emits its own
warning set, so a specific MSVC warning number can differ) or any **runtime** behaviour.
The Windows named-pipe ACL semantics that the m3 assertion was really about need a real
Windows kernel; that verdict stays with the CI. Think of this as the compile-and-ABI
gate that trims most of the back-and-forth, not a replacement for the Windows column.

## Pieces

- `../../cmake/toolchains/windows-clang-cl.cmake` - the cross toolchain.
- `check-windows.sh` - configure and build a CMake source dir with that toolchain.
- `probe.cpp` / `CMakeLists.txt` - a QtCore console target that proves the toolchain
  before any framework target is attempted.

## One-time provisioning

The provisioning needs the network and `sudo`, so it lives in the git-ignored
`.run-for-me.sh` at the repo root:

```
./.run-for-me.sh winsetup
```

That step installs `lld` (for `lld-link`), builds `xwin` and splats the MSVC CRT/SDK,
installs the Windows Qt kit (`aqt ... win64_msvc2022_64`), then runs the probe through
`check-windows.sh` and records the result in `.run-for-me.report.txt`. Get the probe
green first; only then does building the real suites make sense.

## Running the check

Once provisioned, point the environment at the three trees and run the check on any
target dir:

```
export XWIN_DIR="$HOME/.cache/synqt-xwin"                 # the 'xwin splat' output
export QT_WIN="$HOME/Qt-win/6.11.1/msvc2022_64"           # the Windows Qt kit
export QT_HOST="/opt/Qt/6.11.1/gcc_64"                    # the Linux host kit (generators)
export OPENSSL_WIN="$HOME/.cache/synqt-openssl-win/Library" # a Windows OpenSSL prefix

tools/windows-check/check-windows.sh                 # the QtCore probe (no OpenSSL needed)
tools/windows-check/check-windows.sh tests/m3-mesh   # a real suite (needs OPENSSL_WIN)
```

Anything linking `SynQtService` (mesh mutual TLS) does `find_package(OpenSSL REQUIRED)`,
so those targets need a Windows OpenSSL (import libraries and headers) that `OPENSSL_WIN`
points at (the directory holding `include/` and `lib/`). The `winsetup` step fetches the
conda-forge `win-64` OpenSSL with micromamba, which downloads and extracts the
foreign-platform package without running it, into `$HOME/.cache/synqt-openssl-win`. When
`OPENSSL_WIN` is absent, `check-windows.sh` still runs (the QtCore probe needs no OpenSSL)
and a `SynQtService` target simply fails at `find_package(OpenSSL)` with a clear note.

### Expected cross-compile warning

Configuring prints one `CMake Warning`: *"No qtpaths executable found for deployment
purposes"*. This is a cross-compile artifact, not a defect: the Windows kit's
`qtpaths.exe` cannot run on Linux, and the message only concerns Qt's deployment helpers
(`qt_generate_deploy_app_script` and friends), which a compile-and-link gate never calls.
It does not appear on the real Windows CI, where `qtpaths.exe` runs natively. The gate
deliberately does not fail on it; the true `/W4 /WX` warning verdict stays the CI's job.
