# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt doctor``: diagnose the toolchain, certificates, and license obligations."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import (clientbuild, config as configmod, licenses, toolchain,
               version as versionmod)

QT_VERSION = toolchain.QT_VERSION


def report(project_dir: os.PathLike[str] | str,
           qt_license_mode: str = "open_source",
           profile: Optional[str] = None) -> str:
    root = Path(project_dir)
    # The version block leads the report: a question about a build is nearly always a
    # question about which synqt, Qt, and Emscripten produced it, so it belongs before
    # everything else rather than buried in the toolchain section below.
    lines: List[str] = list(versionmod.version_lines())
    lines.append("")
    lines.append("synqt doctor:")

    # The resolved configuration, not the base file: doctor reports on the deployment as
    # it will actually run, and a profile or a SYNQT_ override is part of that.
    resolved = configmod.resolve(root, profile=profile)
    config: Dict[str, Any] = resolved.config
    for source in resolved.sources:
        lines.append(f"  config layer: {source}")

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
    host_qt = toolchain.resolve(root, threads=threads).get("host_qt")
    for entity in config.get("entities", []):
        provider = (entity.get("provider") or {}).get("name")
        if provider in ("postgres", "mysql", "mongodb", "redis"):
            lines.append(f"  Provider '{provider}' on entity '{entity.get('name')}':")
            for line in _provider_dependency_lines(provider, host_qt):
                lines.append("    - " + line)
    return "\n".join(lines)


# The Qt SQL driver plugin each SQL-backed provider loads at run time, by platform file
# name. A provider whose plugin is absent builds and starts fine and then fails on its
# first query, so doctor is the right place to say so.
SQL_DRIVER_PLUGINS = {
    "postgres": ("QPSQL", "qsqlpsql"),
    "mysql": ("QMYSQL", "qsqlmysql"),
}


def _sql_driver_plugin(host_qt: Optional[str], stem: str) -> Optional[Path]:
    """The Qt SQL driver plugin file in the resolved kit, or None when it is not there."""
    if not host_qt:
        return None
    directory = Path(host_qt) / "plugins" / "sqldrivers"
    for name in (f"lib{stem}.so", f"{stem}.dll", f"lib{stem}.dylib", f"{stem}.dylib"):
        candidate = directory / name
        if candidate.is_file():
            return candidate
    return None


def _provider_dependency_lines(provider: str, host_qt: Optional[str]) -> List[str]:
    """What this provider needs beyond synqt's own build, reported honestly.

    `synqt build` compiles SynQt against the client libraries it finds; it does not
    install an engine client, and for the SQL families it does not produce the Qt SQL
    driver plugin either. Saying "synqt build resolves it" was wrong on both counts, and
    wrong in the direction that matters: it sends someone whose queries fail at run time
    looking in the build for a run-time plugin problem.
    """
    if provider in ("mongodb", "redis"):
        library = "the MongoDB C driver (mongo-c-driver)" if provider == "mongodb" \
            else "hiredis"
        return [f"needs {library} present when SynQt is built, or the provider is "
                "compiled out and selecting it fails at startup.",
                "needs its engine reachable over verified TLS (release refuses plaintext)."]

    driver, stem = SQL_DRIVER_PLUGINS[provider]
    lines = [f"needs the {driver} Qt SQL driver plugin at run time, and its engine "
             "reachable over verified TLS (release refuses plaintext)."]
    plugin = _sql_driver_plugin(host_qt, stem)
    if plugin is None:
        lines.append(f"{driver} plugin: not in the resolved Qt kit"
                     + (f" ({host_qt}/plugins/sqldrivers)" if host_qt else "")
                     + "; queries will fail at run time until it is on QT_PLUGIN_PATH.")
    elif provider == "mysql":
        # Deliberately not "present": on a stock kit this file IS the unusable prebuilt
        # one, and reporting it as present would be the reassuring half of the truth.
        lines.append(f"{driver} plugin: a plugin file is in the Qt kit ({plugin}), which "
                     "settles nothing on its own; what matters is the client it was "
                     "built against.")
    else:
        lines.append(f"{driver} plugin: present ({plugin}).")
    if provider == "mysql":
        # The one case where the plugin being present is not good news. Qt's prebuilt
        # QMYSQL is linked against Oracle's libmysqlclient (with its versioned symbols,
        # so no MariaDB Connector/C shim can satisfy it), and conveying that alongside
        # the LGPLv3 Qt modules is not permitted at all (docs/licensing.md). doctor
        # cannot tell the two builds apart from the file alone, so it says so plainly
        # rather than reporting "present" and leaving it there.
        lines.append("the plugin must be built against MariaDB Connector/C (LGPLv2.1). "
                     "Qt's prebuilt QMYSQL links Oracle's GPLv2-only libmysqlclient, "
                     "which cannot be conveyed with the LGPLv3 Qt modules in this entity, "
                     "and will not load against Connector/C either. Rebuild it with "
                     "tools/qmysql-plugin/build-qmysql-plugin.sh.")
    return lines
