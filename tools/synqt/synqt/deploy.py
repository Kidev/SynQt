# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The platform deployment step for the native desktop client, run only when asked.

`synqt build --client desktop` does not deploy. That is deliberate and documented
(docs/desktop.md): signing identities, entitlements, notarization and installer format are not a
framework's to choose, and a half-deployed bundle that looks finished is worse than one that says
what is missing. What the build guarantees is that the step *can* be run -- on macOS that is why
the client is built as an .app bundle at all, since macdeployqt accepts nothing else.

`--deploy` is the opt-in that runs it anyway, for the case where the developer wants a
self-contained tree out of one command and will sign it themselves afterwards. It never signs:
an unsigned .app is still Gatekeeper-blocked, so pretending otherwise would recreate exactly the
"looks finished" failure the default position exists to avoid.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional


class DeployError(Exception):
    """The deploy step could not run. Carries a message meant for the CLI's output."""


def _tool(host_qt: Optional[str], name: str) -> Path:
    if not host_qt:
        raise DeployError(
            f"--deploy needs the host Qt kit to find {name}, and no host Qt was resolved. "
            "Run `synqt doctor` to see what the toolchain resolver found.")
    for candidate in (Path(host_qt) / "bin" / name, Path(host_qt) / "bin" / f"{name}.exe"):
        if candidate.exists():
            return candidate
    raise DeployError(
        f"--deploy could not find {name} in {host_qt}/bin. It ships with the desktop Qt kit; "
        "a kit installed without it cannot deploy.")


def _run(command: List[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        # The tool's own diagnosis, not a generic failure: macdeployqt in particular reports
        # the specific plugin or framework it could not resolve, and that is the whole value
        # of the message.
        detail = (result.stderr or result.stdout or "").strip().splitlines()
        tail = "\n    ".join(detail[-6:]) if detail else "(no output)"
        raise DeployError(f"{Path(command[0]).name} failed:\n    {tail}")
    return result.stdout


def deploy_client(root: Path, name: str, out: Path, resolved: Dict[str, Any],
                  platform: str) -> str:
    """Deploy the installed desktop client in `out`. Returns a one-line note for the summary."""
    host_qt = resolved.get("host_qt")
    if platform == "macos":
        app = out / f"{name}.app"
        if not app.is_dir():
            raise DeployError(f"--deploy found no app bundle at {app}.")
        _run([str(_tool(host_qt, "macdeployqt")), str(app), f"-qmldir={root}"])
        return (f"deployed {app.name} with macdeployqt (unsigned; "
                "add your signing identity before distributing)")
    if platform == "windows":
        exe = out / f"{name}.exe"
        if not exe.is_file():
            raise DeployError(f"--deploy found no executable at {exe}.")
        _run([str(_tool(host_qt, "windeployqt")), "--qmldir", str(root), str(exe)])
        return f"deployed {exe.name} with windeployqt"
    return _deploy_linux(root, name, out, host_qt)


def _deploy_linux(root: Path, name: str, out: Path, host_qt: Optional[str]) -> str:
    """The portable layout: Qt's libraries and QML modules beside the binary, plus a launcher.

    Linux has no official Qt deployment tool, so this is mechanical rather than blessed: it
    copies what the binary actually links (via ldd, filtered to the kit, so system libraries
    such as glibc are deliberately left to the host) plus the kit's QML modules, and writes a
    launcher that points Qt at both. It is the layout docs/desktop.md describes. For a single
    distributable file, linuxdeploy or an AppImage recipe wraps this tree.
    """
    binary = out / name
    if not binary.is_file():
        raise DeployError(f"--deploy found no executable at {binary}.")
    if not host_qt:
        raise DeployError("--deploy needs the host Qt kit to know which libraries are Qt's.")
    kit = Path(host_qt).resolve()

    lib_dir = out / "lib"
    lib_dir.mkdir(exist_ok=True)
    copied = 0
    for line in _run(["ldd", str(binary)]).splitlines():
        if "=>" not in line:
            continue
        target = line.split("=>", 1)[1].strip().split(" ")[0]
        if not target:
            continue
        source = Path(target)
        # Both sides resolved before comparing. Resolving only the kit meant any symlink on
        # the way to it made every Qt library look like a system one, and the deployed tree
        # came out with a launcher and no libraries at all -- a "success" that produces an
        # app which cannot start. A symlinked /opt/Qt is enough to trigger it.
        try:
            real = source.resolve()
        except OSError:
            continue
        if not str(real).startswith(str(kit)):
            continue  # a system library: the host's to provide, not ours to ship
        if real.is_file():
            shutil.copy2(real, lib_dir / source.name, follow_symlinks=True)
            copied += 1

    qml_src = kit / "qml"
    qml_dst = out / "qml"
    if qml_src.is_dir() and not qml_dst.exists():
        shutil.copytree(qml_src, qml_dst, symlinks=True)

    plugins_src = kit / "plugins"
    plugins_dst = out / "plugins"
    if plugins_src.is_dir() and not plugins_dst.exists():
        shutil.copytree(plugins_src, plugins_dst, symlinks=True)

    launcher = out / f"{name}.sh"
    launcher.write_text(
        "#!/usr/bin/env bash\n"
        "# Generated by `synqt build --deploy`. Runs the client against the Qt shipped\n"
        "# beside it rather than whatever the host happens to have installed.\n"
        'here="$(cd "$(dirname "$0")" && pwd)"\n'
        'export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        'export QML2_IMPORT_PATH="$here/qml${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"\n'
        'export QT_PLUGIN_PATH="$here/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"\n'
        f'exec "$here/{name}" "$@"\n')
    launcher.chmod(launcher.stat().st_mode | 0o111)
    return (f"deployed {name} as a portable layout ({copied} Qt libraries, qml/ and plugins/); "
            f"launch through {launcher.name}")
