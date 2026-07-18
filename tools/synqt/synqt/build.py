# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt build``: compile every entity through the pinned toolchain, emit one deployable
directory per entity with an accurate THIRD-PARTY-LICENSES, precompress the client bundle,
and write a dependency-ordered process manifest.

The compilation runs through the generated CMake presets (host kit for services and the
desktop client, the WebAssembly kit for the browser client). This module always emits the
per-entity layout, licenses, precompressed bundle, and manifest; the parts that must stay
accurate as the topology changes, and drives the real cmake build when build files exist.
"""

from __future__ import annotations

import gzip
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml

from . import (appgen, clientbuild, clientcache, licenses, manifest, presets, run,
               toolchain, topologywriter)


class BuildError(Exception):
    """A build error surfaced to the CLI (no traceback for the user)."""


def load_config(project_dir: os.PathLike[str] | str) -> Dict[str, Any]:
    config_path = Path(project_dir) / "synqt.yaml"
    if not config_path.exists():
        raise FileNotFoundError(f"no synqt.yaml in {project_dir}")
    return yaml.safe_load(config_path.read_text()) or {}


def _client_targets(entity: Dict[str, Any], requested: str) -> List[str]:
    declared = entity.get("targets", ["wasm"])
    if requested == "all":
        return declared
    return [requested] if requested in declared else []


def _wasm_runtime_files(wasm_dir: Path) -> List[Path]:
    """The Emscripten runtime + assets to serve (the .js/.wasm/.svg, not Qt's .html: SynQt
    ships its own CSP-clean index shell instead of Qt's inline-handler template)."""
    wanted: List[Path] = []
    for pattern in ("*.js", "*.wasm", "*.svg"):
        # qtlogo.svg exists only for Qt's stock template, which SynQt replaces: shipping it
        # would put Qt's mark (and two precompressed copies of it) in every app's bundle,
        # referenced by nothing.
        wanted += sorted(p for p in wasm_dir.glob(pattern) if p.name != "qtlogo.svg")
    return wanted


def assemble_bundle(wasm_dir: Path, client_dir: Path, config: Dict[str, Any],
                    project_dir: Path) -> int:
    """Assemble the served bundle: copy the WASM runtime + assets, then write SynQt's own
    CSP-clean index.html and external synqt-boot.js (Qt's default template boots from an
    inline handler the edge's strict CSP blocks). Returns the file count."""
    client_dir.mkdir(parents=True, exist_ok=True)
    runtime = _wasm_runtime_files(wasm_dir)
    # The app runtime js is <target>.js; the loader is qtloader.js. The entry symbol the
    # boot script calls is window.<target>_entry.
    app_js = next((p for p in runtime if p.name != "qtloader.js" and p.suffix == ".js"), None)
    target = app_js.stem if app_js else "client"

    count = 0
    for source in runtime:
        shutil.copy2(source, client_dir / source.name)
        count += 1
    (client_dir / "index.html").write_text(
        appgen.render_client_shell(f"{target}.js", config, project_dir))
    (client_dir / "synqt-boot.js").write_text(appgen.render_boot_js(target, config))
    extra = 2
    # Written before the manifest so the worker appears in the manifest's file list and
    # therefore precaches itself along with the rest of the shell.
    if clientcache.uses_service_worker(config):
        (client_dir / "synqt-sw.js").write_text(appgen.render_service_worker_js())
        extra += 1

    # Written last: the manifest lists the assembled bundle, and precompression has not
    # run yet, so the .br/.gz variants are correctly absent from it either way.
    if app_js is not None and (client_dir / f"{target}.wasm").is_file():
        manifest.write(client_dir, f"{target}.wasm")
        return count + extra + 1
    return count + extra


def _desktop_edge_url(config: Dict[str, Any]) -> Optional[str]:
    """The edge URL a native desktop client connects to (build.desktop.edge_url). Unlike the
    WASM client (which reads its edge from the page the edge served it), a desktop app has
    no serving origin, so it is compiled with this URL baked in (SYNQT_EDGE_URL). Returns None
    when unset, leaving the CMake default in place."""
    desktop = ((config.get("build") or {}).get("desktop") or {})
    url = desktop.get("edge_url")
    return url if isinstance(url, str) and url else None


def _run(command: List[str], cwd: Path, verbose: bool) -> None:
    """Run a build step. Quiet by default (cmake's output is noise on a green build); with
    --verbose the command is echoed and its output streams straight through, which is the
    only way to see a compiler error in context rather than the one-line summary below."""
    if verbose:
        print("  $ " + " ".join(str(part) for part in command), flush=True)
        subprocess.run(command, cwd=cwd, check=True)
        return
    subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)


def built_note(host_targets: List[str], client_targets: List[str]) -> str:
    """Name what was compiled, not "every entity": with --entity this note is the only
    thing that says the build was partial, and claiming otherwise is how a stale binary
    gets deployed as a fresh one. Separate from _cmake_build so the wording is testable
    without a Qt toolchain to compile through."""
    built = list(host_targets) + [f"client ({t})" for t in client_targets if t == "wasm"]
    if not built:
        return "nothing to compile."
    return f"compiled {', '.join(built)} through the pinned toolchain."


def _cmake_build(project_dir: Path, resolved: Dict[str, Optional[str]],
                 host_targets: List[str], client_targets: List[str],
                 config: Dict[str, Any], edge_url: Optional[str] = None,
                 verbose: bool = False) -> str:
    """Compile the host targets (services + optional desktop client) and, when the wasm
    client is requested, the browser client through the pinned Emscripten Qt kit. A desktop
    client build bakes in edge_url (build.desktop.edge_url) as SYNQT_EDGE_URL."""
    if not (project_dir / "CMakePresets.json").exists() or not (project_dir / "CMakeLists.txt").exists():
        return ("note: no CMakeLists.txt yet; emitting the deploy layout, licenses, and "
                "manifest; generate the entity build files to compile binaries.")
    need_wasm = "wasm" in client_targets
    if not toolchain.is_complete(resolved, need_wasm=need_wasm):
        return ("note: toolchain incomplete (run 'synqt doctor'); skipped compilation, "
                "emitted the deploy layout and licenses.")
    cmake = resolved["cmake"]
    # Point the host configure at the resolved host Qt kit. The preset carries the
    # provisioned synqt/toolchain path, but a developer with a system Qt (resolved via
    # /opt/Qt or QTDIR) has not populated it; passing the resolved prefix makes the build
    # work either way without editing the preset.
    host_configure = [cmake, "--preset", "host"]
    if resolved.get("host_qt"):
        host_configure.append(f"-DCMAKE_PREFIX_PATH={resolved['host_qt']}")
    if edge_url:
        host_configure.append(f"-DSYNQT_EDGE_URL={edge_url}")
    try:
        if host_targets:
            _run(host_configure, project_dir, verbose)
            build_command = [cmake, "--build", str(project_dir / "build" / "host")]
            for target in host_targets:
                build_command += ["--target", target]
            if verbose:
                build_command.append("--verbose")
            _run(build_command, project_dir, verbose)
        if need_wasm:
            # The WebAssembly client builds through the wasm kit's qt-cmake wrapper (which
            # installs the Emscripten toolchain file); the root CMakeLists guards the
            # service targets behind `if(NOT EMSCRIPTEN)`, so only the client is built.
            qt_cmake = Path(resolved["wasm_qt"]) / "bin" / "qt-cmake"
            # One build directory per kit: qt-cmake's toolchain choice is cached on the
            # first configure, so a shared directory would silently keep the other kit's.
            wasm_dir = project_dir / clientbuild.wasm_build_dir(config)
            # QT_HOST_PATH is passed explicitly, never left to the kit: a cross-compiled Qt has
            # to be told where its host tools (moc, rcc, qmlcachegen) live. The kit bakes in the
            # path from the machine Qt itself was built on (/home/qt/work/install), and aqt
            # rewrites that only when it installs a host-specific kit -- the WebAssembly kit is
            # published host-independently (all_os/wasm), so nothing rewrites it and the
            # configure dies on "please set the QT_HOST_PATH cache variable". We already
            # resolved the host kit, so say so rather than depend on an installer's patching.
            _run([str(qt_cmake), "-S", str(project_dir), "-B", str(wasm_dir),
                  "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                  f"-DQT_HOST_PATH={resolved['host_qt']}"], project_dir, verbose)
            wasm_build = [cmake, "--build", str(wasm_dir)]
            if verbose:
                wasm_build.append("--verbose")
            _run(wasm_build, project_dir, verbose)
            assemble_bundle(wasm_dir, project_dir / "build" / "client", config, project_dir)
        return built_note(host_targets, client_targets)
    except subprocess.CalledProcessError as error:
        # A failed compile ends the command. This used to return the message as a note, which
        # build() then appended to a "Built N entity artifact(s)" summary and printed on the
        # way to exit code 0: `synqt build` reported success, listed artifacts it had not
        # produced, and wrote each one a THIRD-PARTY-LICENSES describing a binary that did not
        # exist. CI only caught it because a later step looked for the bundle on disk.
        raise BuildError(_compile_failure(error, verbose)) from error


# Enough to carry a CMake FATAL_ERROR with its call stack, or a compiler error with the line
# it points at; past that it is scrollback for something --verbose reports better.
_FAILURE_TAIL_LINES = 20


def _compile_failure(error: subprocess.CalledProcessError, verbose: bool) -> str:
    """Explain a failed build step. Without --verbose the output was captured, so the
    message has to carry it: cmake's own last line is 'Configuring incomplete, errors
    occurred!', which names nothing, while the FATAL_ERROR that matters is some lines
    above it. Quote the tail rather than one line."""
    command = " ".join(str(part) for part in error.cmd)
    if verbose:
        # The output already streamed past; repeating a slice of it would only bury it.
        return f"cmake build failed (see the output above): {command}"
    detail = (error.stderr or "").strip().splitlines()
    if not detail:
        return f"cmake build failed with no output captured: {command}"
    tail = "\n".join(f"  {line}" for line in detail[-_FAILURE_TAIL_LINES:])
    return f"cmake build failed: {command}\n{tail}"


def _targets_for(config: Dict[str, Any], client: str) -> Tuple[Optional[Dict[str, Any]],
                                                               List[str], List[str]]:
    """Resolve the host targets (services, plus the client only for a desktop build) and the
    client targets requested. The browser client compiles through the separate wasm kit."""
    client_entity = next((e for e in config.get("entities", []) if e.get("kind") == "client"),
                         None)
    client_targets = _client_targets(client_entity, client) if client_entity else []
    host_targets = [e.get("name") for e in config.get("entities", [])
                    if e.get("kind") != "client" and e.get("name")]
    if client_entity and "desktop" in client_targets:
        host_targets.append(client_entity.get("name"))
    return client_entity, host_targets, client_targets


def compile_incremental(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
                        client: str = "wasm") -> Tuple[str, List[str], List[str]]:
    """Regenerate the app from the topology and run an incremental cmake build, then
    reinstall the host binaries so a restarted service picks up the new build. Used by
    ``synqt dev``'s watcher (cmake --build is incremental). Returns the compile note plus
    the host and client target lists that were built."""
    root = Path(project_dir).resolve()
    resolved = toolchain.resolve(root, threads=clientbuild.client_threads(config))
    appgen.generate(root, config)
    presets.write(root, config)
    topologywriter.write(root, config)  # the machine topology each service reads at startup
    client_entity, host_targets, client_targets = _targets_for(config, client)
    edge_url = _desktop_edge_url(config) if "desktop" in client_targets else None
    note = _cmake_build(root, resolved, host_targets, client_targets, config=config,
                        edge_url=edge_url)
    build_dir = root / "build"
    for entity in config.get("entities", []):
        name = entity.get("name")
        if not name:
            continue
        if entity.get("kind") == "client":
            if "desktop" in _client_targets(entity, client):
                _install_binary(build_dir, name,
                                build_dir / "client-desktop" / desktop_platform())
        else:
            _install_binary(build_dir, name, build_dir / name)
    return note, host_targets, client_targets


def desktop_platform() -> str:
    """The `build/client-desktop/<platform>/` folder for the host being built on.

    A desktop client is native, so it is always built on the platform it targets (see
    docs/desktop.md, which names these three folders). The name comes from the host rather
    than from config for that reason: there is no cross-building a desktop client here, so a
    configurable value could only ever disagree with what was actually produced.

    One host-name function, not two: the toolchain resolver needs the same answer to pick
    the host kit directory, and the way both of these went wrong was a second copy of a
    platform assumption drifting from the first.
    """
    return toolchain.host_platform()


def _install_binary(build_dir: Path, entity_name: str, dest: Path) -> bool:
    """Copy a compiled host binary into its deploy directory so `synqt serve` finds it
    alongside its THIRD-PARTY-LICENSES. Returns True when a binary was installed.

    The suffix is resolved rather than assumed (run.host_binary): Windows links `<name>.exe`, so
    looking only for the bare name there finds nothing, and this returns False for a binary that
    built perfectly well -- a deploy directory that is silently missing its executable.
    """
    compiled = run.host_binary(build_dir.parent, entity_name)
    if compiled is None:
        return False
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(compiled, dest / compiled.name)
    return True


# Everything on the first-visit critical path that compresses well. The wasm dominates,
# but the Emscripten glue .js is the second-largest asset and was previously shipped raw.
# The .gz/.br variants themselves match none of these, so a second pass is a no-op rather
# than a way to produce client.wasm.gz.gz.
_COMPRESSIBLE = ("*.wasm", "*.js", "*.html", "*.json", "*.svg")


def precompress(client_dir: Path) -> int:
    """Brotli + gzip every compressible bundle asset so the edge can serve the smaller
    copy. The edge picks per request from Accept-Encoding; these are additions beside the
    original, never replacements."""
    count = 0
    for pattern in _COMPRESSIBLE:
        for asset in sorted(Path(client_dir).glob(pattern)):
            data = asset.read_bytes()
            asset.with_name(asset.name + ".gz").write_bytes(gzip.compress(data, 9))
            try:
                import brotli
                asset.with_name(asset.name + ".br").write_bytes(brotli.compress(data))
            except ImportError:
                pass
            count += 1
    return count


def write_process_manifest(config: Dict[str, Any], build_dir: Path) -> Path:
    """A dependency-ordered start plan: owners before consumers, only the edge public."""
    order = run.startup_order(config)
    edges = {e.get("name") for e in config.get("entities", [])
             if e.get("capability") == "web_edge" or e.get("web_edge")}
    processes = [{
        "entity": name,
        "binary": f"build/{name}/{name}",
        "bind": "public" if name in edges else "loopback",
        "mesh_cert": f"synqt/mesh/{name}.crt",
        "mesh_key": f"synqt/mesh/{name}.key",
        "ca_cert": "synqt/mesh/ca.crt",
    } for name in order]
    manifest = {"start_order": order, "processes": processes,
                "client_served_from": "build/client/"}
    path = build_dir / "process-manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return path


def _selected_entities(config: Dict[str, Any], entity: Optional[str]) -> List[Dict[str, Any]]:
    """The entities this build acts on: all of them, or the one `--entity` names.

    A name that matches nothing is an error, not an empty build. Silently producing
    "Built 0 entity artifact(s)" for a typo is the kind of success that wastes an
    afternoon.
    """
    entities = [e for e in config.get("entities", []) if isinstance(e, dict) and e.get("name")]
    if entity is None:
        return entities
    selected = [e for e in entities if e.get("name") == entity]
    if not selected:
        known = ", ".join(sorted(e["name"] for e in entities)) or "none"
        raise BuildError(f"no entity named '{entity}' in this project (declared: {known})")
    return selected


def build(project_dir: os.PathLike[str] | str, *, release: bool = True,
          client: str = "wasm", qt_license_mode: str = "open_source",
          entity: Optional[str] = None, threads: Optional[str] = None,
          verbose: bool = False) -> str:
    # Resolve to an absolute path: the cmake invocations below run with cwd set to the
    # project dir, so a relative --project-dir would otherwise be joined against itself.
    root = Path(project_dir).resolve()
    config = clientbuild.with_threads(load_config(root), threads)
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)
    resolved = toolchain.resolve(root, threads=clientbuild.client_threads(config))
    selected = _selected_entities(config, entity)

    # Regenerate the app from the current topology so a connect-point change is reflected
    # in the CMakeLists, the CMakePresets, and the per-entity main before we compile. This
    # keeps `synqt build` self-sufficient on any project (a docs example, a hand-authored
    # tree), not only one scaffolded by `synqt new`.
    appgen.generate(root, config)
    presets.write(root, config)
    topologywriter.write(root, config)  # the machine topology each service reads at startup

    # Only among the selected entities: `--entity web` must not compile the client too.
    client_entity = next((e for e in selected if e.get("kind") == "client"), None)
    client_targets = _client_targets(client_entity, client) if client_entity else []

    # Host targets: every service entity, plus the client only when a desktop build is
    # requested (the browser client compiles through the separate wasm kit).
    host_targets = [e.get("name") for e in selected if e.get("kind") != "client"]
    if client_entity and "desktop" in client_targets:
        host_targets.append(client_entity.get("name"))
    edge_url = _desktop_edge_url(config) if "desktop" in client_targets else None
    compile_note = _cmake_build(root, resolved, host_targets, client_targets,
                                config=config, edge_url=edge_url, verbose=verbose)

    produced: List[str] = []
    for entity in selected:
        name = entity.get("name")
        if entity.get("kind") == "client":
            for target in _client_targets(entity, client):
                folder = "client" if target == "wasm" else "client-desktop"
                out = build_dir / folder
                if target == "desktop":
                    # The host's own folder (windows/, macos/, linux/ per docs/desktop.md). A
                    # desktop client is native, so the only one this build can fill is this
                    # host's; the others come from that platform's own run of the same command.
                    out = out / desktop_platform()
                out.mkdir(parents=True, exist_ok=True)
                if target == "desktop":
                    (out.parent / "DEPLOY.txt").write_text(
                        "Run the platform deploy step: windeployqt / macdeployqt, or the "
                        "portable Linux layout here (binary + Qt libs).\n")
                (out / "THIRD-PARTY-LICENSES").write_text(
                    licenses.generate(entity, target=target, qt_license_mode=qt_license_mode))
                # The desktop client compiles on the host; place it beside its licenses.
                if target == "desktop":
                    _install_binary(build_dir, name, out)
                produced.append(f"build/{folder}/ ({target})")
        else:
            out = build_dir / name
            out.mkdir(parents=True, exist_ok=True)
            (out / "THIRD-PARTY-LICENSES").write_text(
                licenses.generate(entity, qt_license_mode=qt_license_mode))
            _install_binary(build_dir, name, out)  # so `synqt serve` can launch it
            produced.append(f"build/{name}/")

    # Only when this build produced the bundle: with --entity web the client dir may still
    # hold an older bundle, and recompressing it would report work this build did not do.
    built_wasm_client = "wasm" in client_targets and (build_dir / "client").exists()
    compressed = precompress(build_dir / "client") if built_wasm_client else 0
    write_process_manifest(config, build_dir)

    summary = [f"Built {len(produced)} entity artifact(s) ({'release' if release else 'debug'}):"]
    summary += [f"  - {item}" for item in produced]
    summary.append(f"  {compile_note}")
    if compressed:
        summary.append(f"  precompressed {compressed} bundle file(s) (Brotli + gzip).")
    summary.append("  wrote build/process-manifest.json (owners start before consumers).")

    # Each licence reminder belongs to an artifact this build actually produced. With
    # --entity database, warning about a client that was not built teaches the reader to
    # skim past the warning, which is how the one that matters gets missed.
    if qt_license_mode == "open_source":
        notices: List[str] = []
        if client_targets:
            notices.append(licenses.CLIENT_GPL_WARNING)
        if any(e.get("capability") == "web_edge" or e.get("web_edge") for e in selected):
            notices.append("Note: distributing the edge binary triggers GPLv3 (Qt HTTP "
                           "Server / Network Authorization). See docs/licensing.md.")
        if notices:
            summary += [""] + notices
    return "\n".join(summary)
