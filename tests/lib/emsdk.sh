# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Find and activate an Emscripten SDK. Sourced: `. "$REPO_ROOT/tests/lib/emsdk.sh"`.
# The prebuilt WASM kits resolve their toolchain file through EMSDK.

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
    echo "         ~/emsdk/emsdk install 5.0.5 && ~/emsdk/emsdk activate 5.0.5" >&2
    return 1
}
