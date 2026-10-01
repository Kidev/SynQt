<!--
SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
SPDX-License-Identifier: Apache-2.0
-->

# Windows cross-compile gate

A local pre-push check that compiles the SynQt native targets for the Windows MSVC ABI
from a Linux workstation, so a Windows code path that does not build is caught here
instead of on a CI round trip. It uses `clang-cl` and `lld-link` against the Microsoft CRT
and Windows SDK that [`xwin`](https://github.com/Jake-Shadle/xwin) fetches, links the
Windows Qt kit, and runs the code generators (`moc`, `rcc`, `repc`) from the Linux host Qt
kit.

## What it does and does not catch

It catches `Q_OS_WIN` blocks that do not compile, wrong or missing Windows includes and
types, MSVC STL and SDK header incompatibilities, named-pipe and ACL API usage that does
not compile or link, and ABI-level mistakes. A target that builds here builds under the
CI's `cl.exe`.

It does not catch the exact `cl.exe /W4 /WX` warning verdict, since clang emits its own
warning set and a specific MSVC warning number can differ, and on its own it runs nothing.
A built test can run under Wine (below), which answers for Qt's own Windows code paths,
such as `QSettings` in the registry. The Windows named-pipe ACL semantics that the mesh
assertion is about need a real Windows kernel, so that verdict stays with the CI. This is
a compile-and-ABI gate that trims most of the back-and-forth, and it does not replace the
Windows column.

## Pieces

- `../../cmake/toolchains/windows-clang-cl.cmake`: the cross toolchain.
- `check-windows.sh`: configure and build a CMake source dir with that toolchain.
- `probe.cpp` / `CMakeLists.txt`: a QtCore console target that proves the toolchain
  before any framework target is attempted.

## One-time provisioning

Four pieces, installed once. `clang-cl` and `lld-link` come with the distribution's
clang and lld packages. `xwin` fetches the MSVC CRT and the Windows SDK from Microsoft's
own download servers under their license. The Windows Qt kit needs the `aqtinstall` commit
the CLI pins (`toolchain.AQT_REQUIREMENT`), because no released aqt can address the layout
Qt publishes that kit under. The Windows OpenSSL comes from conda-forge's `win-64`
package, which micromamba downloads and extracts without running anything from it.

```sh
sudo pacman -S --needed clang lld            # or: sudo apt-get install clang lld
cargo install xwin
xwin --accept-license splat --output ~/.cache/synqt-xwin
pip install "$(python3 -c 'import sys; sys.path.insert(0, "tools/synqt"); from synqt import toolchain; print(toolchain.AQT_REQUIREMENT)')"
aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 \
    -m qthttpserver qtnetworkauth qtremoteobjects qtwebsockets -O ~/Qt-win
micromamba create -y -p ~/.cache/synqt-openssl-win --platform win-64 -c conda-forge openssl
```

Then run the probe through `check-windows.sh`, as below, and get it passing first.
Building the real suites only makes sense after that.

## Running the check

Once provisioned, point the environment at the three trees and run the check on any
target dir:

```
export XWIN_DIR="$HOME/.cache/synqt-xwin"                 # the 'xwin splat' output
export QT_WIN="$HOME/Qt-win/6.12.0/msvc2022_64"           # the Windows Qt kit
export QT_HOST="/opt/Qt/6.12.0/gcc_64"                    # the Linux host kit (generators)
export OPENSSL_WIN="$HOME/.cache/synqt-openssl-win/Library" # a Windows OpenSSL prefix

tools/windows-check/check-windows.sh                 # the QtCore probe (no OpenSSL needed)
tools/windows-check/check-windows.sh tests/mesh   # a real suite (needs OPENSSL_WIN)
```

Anything linking `SynQtService` (mesh mutual TLS) calls
`find_package(OpenSSL REQUIRED)`, so those targets need a Windows OpenSSL, meaning import
libraries and headers, in the directory `OPENSSL_WIN` points at (the one holding
`include/` and `lib/`): the `micromamba` line under provisioning puts one in
`$HOME/.cache/synqt-openssl-win`. Without `OPENSSL_WIN`, `check-windows.sh` still
runs, since the QtCore probe needs no OpenSSL, and a `SynQtService` target fails at
`find_package(OpenSSL)` with a clear note.

### Expected cross-compile warning

Configuring prints one `CMake Warning`: *"No qtpaths executable found for deployment
purposes"*. It is a cross-compile artifact and not a defect. The Windows kit's
`qtpaths.exe` cannot run on Linux, and the message concerns only Qt's deployment helpers
(`qt_generate_deploy_app_script` and friends), which a compile-and-link gate never calls.
It does not appear on the real Windows CI, where `qtpaths.exe` runs natively, and the gate
does not fail on it. The `/W4 /WX` warning verdict stays the CI's job.

## Running a test under Wine

A test built here runs under Wine with the Windows kit's DLLs on `WINEPATH`. A test with a
scene graph also needs the kit's plugins and QML imports, and the offscreen platform:

```sh
cd build/win-check/tests_privacy
export WINEPATH="$QT_WIN/bin"
export QT_PLUGIN_PATH="$QT_WIN/plugins" QML_IMPORT_PATH="$QT_WIN/qml"
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
wine tst_privacy.exe -o result.txt,txt    # Wine's console drops QtTest's own output
```
