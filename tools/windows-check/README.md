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

It cannot build a target that links `Qt6::Quick`. Quick pulls in `Qt6::OpenGL`, whose
`WrapOpenGL` dependency resolves to the Linux box's own `/usr/include`, and the host GL
headers then collide with the MSVC CRT (a `stdint.h` typedef redefinition, and on from
there). Nothing is wrong with the code when that happens, and the real Windows column has
to check that target instead. `tests/graphics` is the one suite this affects:
`tst_graphics` builds here and `tst_softwarebackend` does not.

It does not catch the exact `cl.exe /W4 /WX` warning verdict, since clang emits its own
warning set and a specific MSVC warning number can differ, and it catches no runtime
behaviour at all. The Windows named-pipe ACL semantics that the mesh assertion is about
need a real Windows kernel, so that verdict stays with the CI. This is a compile-and-ABI
gate that trims most of the back-and-forth, and it does not replace the Windows column.

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
tools/windows-check/check-windows.sh tests/m3-mesh   # a real suite (needs OPENSSL_WIN)
```

Anything linking `SynQtService` (mesh mutual TLS) calls
`find_package(OpenSSL REQUIRED)`, so those targets need a Windows OpenSSL, meaning import
libraries and headers, in the directory `OPENSSL_WIN` points at (the one holding
`include/` and `lib/`). The `winsetup` step fetches the conda-forge `win-64` OpenSSL with
micromamba, which downloads and extracts the foreign-platform package without running it,
into `$HOME/.cache/synqt-openssl-win`. Without `OPENSSL_WIN`, `check-windows.sh` still
runs, since the QtCore probe needs no OpenSSL, and a `SynQtService` target fails at
`find_package(OpenSSL)` with a clear note.

### Expected cross-compile warning

Configuring prints one `CMake Warning`: *"No qtpaths executable found for deployment
purposes"*. It is a cross-compile artifact and not a defect. The Windows kit's
`qtpaths.exe` cannot run on Linux, and the message concerns only Qt's deployment helpers
(`qt_generate_deploy_app_script` and friends), which a compile-and-link gate never calls.
It does not appear on the real Windows CI, where `qtpaths.exe` runs natively, and the gate
does not fail on it. The `/W4 /WX` warning verdict stays the CI's job.
