# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Find and activate an Emscripten SDK for the harnesses that cross-compile a WASM client.
#
# Sourced, not executed: `. "$REPO_ROOT/tests/lib/emsdk.sh"`.
#
# The prebuilt Qt WASM kits are host-independent (aqt's `all_os wasm`), which means the
# chainload toolchain path inside them is whatever Qt's own build machine had:
# `C:/Utils/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`. Nothing rewrites
# it at install time. Qt resolves it at configure time from the EMSDK environment variable
# instead, so a shell that has not activated an emsdk configures against a Windows path that
# cannot exist and dies with "Cannot find the toolchain file Emscripten.cmake" followed by "No
# CMAKE_CXX_COMPILER could be found" -- which reads like a missing compiler rather than like an
# unsourced environment.
#
# CI activates emsdk in its own step before calling these scripts, so only a developer running
# one by hand meets that failure. Doing it here makes the scripts self-sufficient the same way
# QT_HOST_PATH does.

synqt_activate_emsdk() {
    if [ -n "${EMSDK:-}" ] && [ -f "${EMSDK:-}/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        . "${EMSDK}/emsdk_env.sh" >/dev/null 2>&1
        return 0
    fi
    for _synqt_emsdk in "$HOME/emsdk" /opt/emsdk /usr/local/emsdk /usr/lib/emsdk; do
        if [ -f "$_synqt_emsdk/emsdk_env.sh" ]; then
            # shellcheck disable=SC1090
            . "$_synqt_emsdk/emsdk_env.sh" >/dev/null 2>&1
            return 0
        fi
    done
    echo "error: no Emscripten SDK found." >&2
    echo "       Looked at \$EMSDK, ~/emsdk, /opt/emsdk, /usr/local/emsdk, /usr/lib/emsdk." >&2
    echo "       Without one the Qt WASM kit configures against the Windows path baked into" >&2
    echo "       it and fails claiming there is no C++ compiler. Install the pinned version:" >&2
    echo "         git clone https://github.com/emscripten-core/emsdk.git ~/emsdk" >&2
    echo "         ~/emsdk/emsdk install 4.0.7 && ~/emsdk/emsdk activate 4.0.7" >&2
    return 1
}
