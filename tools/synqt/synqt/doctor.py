# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt doctor``: diagnose the toolchain, certificates, and license obligations."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

import yaml

from . import clientbuild, licenses, toolchain

QT_VERSION = toolchain.QT_VERSION


def report(project_dir: os.PathLike[str] | str,
           qt_license_mode: str = "open_source") -> str:
    root = Path(project_dir)
    lines: List[str] = ["synqt doctor:"]

    config: Dict[str, Any] = {}
    config_path = root / "synqt.yaml"
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text()) or {}

    # Qt license mode and the resulting obligations.
    lines.append(f"  Qt license mode: {qt_license_mode}")
    if qt_license_mode == "open_source":
        lines.append("    - client (WASM) and web edge are GPLv3; pure services are LGPLv3.")
        lines.append("    - " + licenses.CLIENT_GPL_WARNING)
    else:
        lines.append("    - commercial: entities may be proprietary; no GPL obligation.")

    # Client build mode: threading and the cross-origin isolation it implies.
    threads = clientbuild.client_threads(config)
    isolated = clientbuild.cross_origin_isolation(config)
    lines.append(f"  Client build: {threads}-threaded WebAssembly"
                 + (" (cross-origin isolated: COOP/COEP + worker-src emitted)" if isolated
                    else "; not cross-origin isolated"))
    if threads == "multi":
        lines.append("    - the multi-threaded client runs only where cross-origin isolation "
                     "is available; the edge serves the headers automatically.")

    # Where the client sends diagnostic output (build.client_logging).
    logging_mode = str((config.get("build") or {}).get("client_logging") or "").lower()
    if logging_mode in ("console", "qt", "none"):
        described = {"console": "routed to the browser console",
                     "qt": "left to Qt's default handler",
                     "none": "debug/info dropped, warnings and above kept"}[logging_mode]
        lines.append(f"  Client logging: {logging_mode} ({described}).")
    else:
        lines.append("  Client logging: default (console in a debug build, dropped in release "
                     "so console.log never ships).")

    # Toolchain (resolved from synqt/toolchain, then a system install).
    for line in toolchain.report(root, threads=threads).splitlines():
        lines.append("  " + line)

    # Mesh certificates vs the topology.
    mesh = root / "synqt" / "mesh"
    lines.append("  Mesh certificates:")
    if not (mesh / "ca.crt").exists():
        lines.append("    - no production CA (run 'synqt mesh init'); 'synqt dev' uses a throwaway dev CA.")
    for entity in config.get("entities", []):
        if entity.get("kind") == "client":
            continue
        name = entity.get("name")
        have = (mesh / f"{name}.crt").exists()
        lines.append(f"    - {name}: "
                     + ("certificate present" if have else "no certificate (run 'synqt mesh cert %s')" % name))

    # Provider engines/drivers.
    for entity in config.get("entities", []):
        provider = (entity.get("provider") or {}).get("name")
        if provider in ("postgres", "mysql", "mongodb", "redis"):
            lines.append(f"  Provider '{provider}' on entity '{entity.get('name')}': "
                         "needs its engine client/driver at build (synqt build resolves it).")
    return "\n".join(lines)
