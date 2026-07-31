# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt build``: compile every entity through the pinned toolchain, emit one deployable
directory per entity with an accurate THIRD-PARTY-LICENSES, precompress the client bundle,
and write a dependency-ordered process manifest.

The compilation runs through the generated CMake presets (host kit for services and the
desktop client, the WebAssembly kit for the browser client). This module always emits the
per-entity layout, licenses, precompressed bundle, and manifest. The parts that must stay
accurate as the topology changes, and drives the real cmake build when build files exist.
"""

from __future__ import annotations

import gzip
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import (appgen, appmodel, clientbuild, clientcache, clientmodules, clientshell,
               config as configmod, deploy as deploymod, licenses, manifest, presets,
               profiles, run, toolchain, topologywriter, writer)


class BuildError(Exception):
    """A build error surfaced to the CLI (no traceback for the user)."""


def load_config(project_dir: os.PathLike[str] | str,
                profile: Optional[str] = None) -> Dict[str, Any]:
    """The effective configuration for a build. synqt.yaml under its profile and the
    environment layer (see `config.resolve`)."""
    return configmod.load(project_dir, profile=profile, required=True)


def _client_targets(entity: Dict[str, Any], requested: str) -> List[str]:
    declared = appmodel.client_targets(entity)
    if requested == "all":
        return declared
    return [requested] if requested in declared else []


def _wasm_runtime_files(wasm_dir: Path, target: Optional[str] = None) -> List[Path]:
    """The Emscripten runtime + assets to serve (the .js/.wasm/.svg, not Qt's .html: SynQt
    ships its own CSP-clean index shell instead of Qt's inline-handler template).

    With `target`, only that target's own artifacts plus the files every client shares
    (qtloader.js and any assets). One Emscripten build directory holds every client
    entity's `<target>.js` and `<target>.wasm` side by side, so a project with two clients
    would otherwise copy both into each bundle and pick whichever sorted first as the
    entry point.
    """
    shared = {"qtloader.js"}
    wanted: List[Path] = []
    for pattern in ("*.js", "*.wasm", "*.svg"):
        # qtlogo.svg exists only for Qt's stock template, which SynQt replaces: shipping it
        # would put Qt's mark (and two precompressed copies of it) in every app's bundle,
        # referenced by nothing.
        for path in sorted(wasm_dir.glob(pattern)):
            if path.name == "qtlogo.svg":
                continue
            if (target is not None and path.suffix in (".js", ".wasm")
                    and path.stem != target and path.name not in shared):
                continue
            wanted.append(path)
    return wanted


def client_bundle_targets(config: Dict[str, Any]) -> Dict[str, str]:
    """Every client entity to assemble a served bundle for, mapped to where it goes.

    Keyed by entity name, which is also the CMake target name, so the caller can pick that
    target's runtime files out of one Emscripten build directory. A desktop-only client is
    absent: it produces an executable, and there is no bundle to serve.

    The value is the same `build/client` a single-client project has always used, so the
    documentation, the generated compose files and the deploy scripts all keep naming a
    path that is still there.
    """
    return {str(entity.get("name") or ""): appmodel.bundle_output_dir(config, entity)
            for entity in appmodel.entities(config)
            if appmodel.is_client(entity) and "wasm" in appmodel.client_targets(entity)}


def assemble_bundle(wasm_dir: Path, client_dir: Path, config: Dict[str, Any],
                    project_dir: Path, target: Optional[str] = None) -> int:
    """Assemble the served bundle. Copy the WASM runtime + assets, then write SynQt's own
    CSP-clean index.html and external synqt-boot.js (Qt's default template boots from an
    inline handler the edge's strict CSP blocks). Returns the file count."""
    client_dir.mkdir(parents=True, exist_ok=True)
    runtime = _wasm_runtime_files(wasm_dir, target)
    # The app runtime js is <target>.js. The loader is qtloader.js. The entry symbol the
    # boot script calls is window.<target>_entry.
    app_js = next((p for p in runtime if p.name != "qtloader.js" and p.suffix == ".js"), None)
    if target is None:
        target = app_js.stem if app_js else "client"

    count = 0
    for source in runtime:
        shutil.copy2(source, client_dir / source.name)
        count += 1
    (client_dir / "index.html").write_text(
        clientshell.render_client_shell(f"{target}.js", config, project_dir))
    writer.write_if_changed(client_dir / "synqt-boot.js",
                            clientshell.render_boot_js(target, config))
    extra = 2
    # Written before the manifest so the worker appears in the manifest's file list and
    # therefore precaches itself along with the rest of the shell.
    if clientcache.uses_service_worker(config):
        writer.write_if_changed(client_dir / "synqt-sw.js",
                                clientshell.render_service_worker_js(target))
        extra += 1

    # Written last. The manifest lists the assembled bundle, and precompression has not
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
    """Run a build step. Quiet by default (cmake's output is noise on a green build). With
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


def _preset_value(project_dir: Path, preset: str, read: Callable[[Dict[str, Any]], Any]) -> Any:
    """What `read` finds on a configure preset, following `inherits` until one answers."""
    presets_file = Path(project_dir) / "CMakePresets.json"
    try:
        document = json.loads(presets_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    by_name = {entry.get("name"): entry
               for entry in document.get("configurePresets", [])
               if isinstance(entry, dict)}
    seen: set[str] = set()
    while preset and preset in by_name and preset not in seen:
        seen.add(preset)  # a malformed inherits cycle must not hang the build
        entry = by_name[preset]
        value = read(entry)
        if value:
            return value
        inherits = entry.get("inherits")
        preset = inherits[0] if isinstance(inherits, list) and inherits else inherits
    return None


def _preset_generator(project_dir: Path, preset: str) -> Optional[str]:
    """The generator a configure preset names, following `inherits`. None when the preset
    leaves it to CMake's per-platform default, in which case there is nothing to compare."""
    generator = _preset_value(project_dir, preset, lambda entry: entry.get("generator"))
    return str(generator) if generator else None


def _cached_generator(build_dir: Path) -> Optional[str]:
    """The generator an existing CMake cache was configured with, or None."""
    cache = build_dir / "CMakeCache.txt"
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None


# Where a configured tree remembers which Qt it was made with. CMake reads a toolchain file
# once and a package directory until it goes missing, so none of them follows a change of kit.
_KIT_CACHE_KEYS = ("CMAKE_TOOLCHAIN_FILE", "Qt6_DIR", "QT_HOST_PATH_CMAKE_DIR")


def _cached_values(build_dir: Path, keys: Tuple[str, ...]) -> Dict[str, str]:
    values: Dict[str, str] = {}
    try:
        lines = (build_dir / "CMakeCache.txt").read_text(
            encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return values
    for line in lines:
        name, _, rest = line.partition(":")
        if name in keys and "=" in rest:
            values[name] = rest.split("=", 1)[1].strip()
    return values


def _real(path: Path) -> Path:
    try:
        return path.resolve()
    except OSError:
        return path


def _kits_asked_for(configure: List[str], project_dir: Path) -> List[Path]:
    """Every Qt kit this configure command names, resolved.

    The wasm kit through its own qt-cmake wrapper, the host kit through QT_HOST_PATH or
    CMAKE_PREFIX_PATH on the command line, and a preset's prefix path and toolchain file.
    Resolved, because a project names its kits through the links under synqt/toolchain.
    """
    kits: List[Path] = []
    if configure and Path(configure[0]).name.startswith("qt-cmake"):
        kits.append(Path(configure[0]).parent.parent)
    for part in configure:
        for flag in ("-DQT_HOST_PATH=", "-DCMAKE_PREFIX_PATH="):
            if part.startswith(flag):
                kits += [Path(entry) for entry in part[len(flag):].split(";") if entry]
    if "--preset" in configure:
        preset = configure[configure.index("--preset") + 1]

        def expand(value: str) -> str:
            return value.replace("${sourceDir}", str(project_dir))

        def prefix(entry: Dict[str, Any]) -> Any:
            value = (entry.get("cacheVariables") or {}).get("CMAKE_PREFIX_PATH")
            return value.get("value") if isinstance(value, dict) else value

        prefix_path = _preset_value(project_dir, preset, prefix)
        if prefix_path:
            kits += [Path(expand(entry)) for entry in str(prefix_path).split(";") if entry]
        toolchain = _preset_value(project_dir, preset, lambda entry: entry.get("toolchainFile"))
        if toolchain:
            # <kit>/lib/cmake/Qt6/qt.toolchain.cmake
            kits.append(Path(expand(str(toolchain))).parents[3])
    return [_real(kit) for kit in kits]


def _foreign_kit(configure: List[str], build_dir: Path, project_dir: Path) -> Optional[str]:
    """A path the cache holds into a Qt kit this configure does not use, or None."""
    kits = _kits_asked_for(configure, project_dir)
    if not kits:
        return None
    for value in _cached_values(build_dir, _KIT_CACHE_KEYS).values():
        if not value or value.endswith("-NOTFOUND"):
            continue
        cached = _real(Path(value))
        if not any(cached == kit or kit in cached.parents for kit in kits):
            return value
    return None


def _clear_incompatible_cache(configure: List[str], build_dir: Path,
                              project_dir: Path) -> Optional[str]:
    """Delete a CMake cache this configure cannot reuse, so the configure below can succeed.
    Returns a line to report, or None.

    Two caches cannot be reused. One made by a different generator than the preset asks for,
    which CMake refuses outright ("Does not match the generator used previously"). And one
    made against a different Qt kit: its toolchain file and Qt6_DIR are read once and kept,
    so after the pinned Qt moves, every configure of that tree fails with "could not find a
    configuration file for package Qt6 that is compatible", naming the old kit and not the
    directory to delete. Either way it is the build directory synqt owns and would have
    created itself, so the fix is synqt's to apply rather than an error to hand back.

    Only the cache and CMakeFiles go. Build outputs are left alone, so this costs a
    reconfigure and a rebuild of what changed, not the whole tree.
    """
    note = None
    if "--preset" in configure:
        wanted = _preset_generator(project_dir, configure[configure.index("--preset") + 1])
        existing = _cached_generator(build_dir)
        if wanted and existing and wanted != existing:
            note = (f"note: {build_dir} was configured with {existing} and the preset now "
                    f"asks for {wanted}; reconfiguring it from scratch.")
    if note is None:
        foreign = _foreign_kit(configure, build_dir, project_dir)
        if foreign:
            note = (f"note: {build_dir} was configured against {foreign}, which is not the "
                    f"Qt kit this build uses; reconfiguring it from scratch.")
    if note is None:
        return None
    shutil.rmtree(build_dir / "CMakeFiles", ignore_errors=True)
    try:
        (build_dir / "CMakeCache.txt").unlink()
    except OSError:
        return None
    return note


def _configure_if_needed(configure: List[str], build_dir: Path, project_dir: Path,
                         verbose: bool) -> bool:
    """Configure the build directory, unless it is already configured with this exact
    command. Returns whether cmake was run.

    Skipping is safe because it is not a shortcut around a stale configure. The generator
    now writes only files whose content changed (synqt.writer), so an unchanged
    `CMakeLists.txt` keeps its modification time, and the generator itself is what the
    build system watches. Ninja re-runs cmake on its own the moment one of those files
    does change. What is left to skip is the case where nothing changed at all, where
    cmake re-derives an identical build graph.

    The stamp records the command, not only the fact of configuring, so a build with a
    different Qt kit or a different `-DSYNQT_EDGE_URL` configures again rather than
    silently inheriting the cache from the last one. It records `CMakePresets.json` with
    it, because that file is read when cmake is invoked and is not one of the inputs the
    generated build graph watches. Ninja re-runs cmake for a changed `CMakeLists.txt` and
    would not notice a preset that moved the build type or added a cache variable.
    """
    stamp = build_dir / ".synqt-configure"
    presets_file = Path(project_dir) / "CMakePresets.json"
    presets_text = presets_file.read_text(encoding="utf-8") if presets_file.is_file() else ""
    command_line = "\n".join(str(part) for part in configure) + "\n--presets--\n" + presets_text
    if (build_dir / "CMakeCache.txt").is_file():
        try:
            if stamp.read_text(encoding="utf-8") == command_line:
                return False
        except OSError:
            pass  # never configured through this path, or the stamp is gone: configure.
    note = _clear_incompatible_cache(configure, build_dir, project_dir)
    if note:
        print(note)
    _run(configure, project_dir, verbose)
    # Written only after a configure that succeeded, so a failed one is retried rather
    # than remembered as done.
    build_dir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(command_line, encoding="utf-8")
    return True


def _preset_name(environment: str, profile_name: str, dev_tools: bool) -> str:
    """Which generated configure preset this build uses.

    `host` and `wasm` are the default profile, which is debug. The others are written beside
    them by presets.write(). The development tree is its own preset rather than an option on
    another one, so that nothing reaches SYNQT_DEV_TOOLS=ON without naming it.
    """
    if dev_tools:
        return f"{environment}-dev"
    if profile_name == "debug":
        return environment
    return f"{environment}-{profile_name}"


def _cmake_build(project_dir: Path, resolved: Dict[str, Any],
                 host_targets: List[str], client_targets: List[str],
                 config: Dict[str, Any], edge_url: Optional[str] = None,
                 verbose: bool = False, profile_name: str = "debug",
                 custom_type: str = "", strip: bool = False,
                 dev_tools: bool = False) -> str:
    """Compile the host targets (services + optional desktop client) and, when the wasm
    client is requested, the browser client through the pinned Emscripten Qt kit. A desktop
    client build bakes in edge_url (build.desktop.edge_url) as SYNQT_EDGE_URL."""
    root = Path(project_dir)
    generated = appmodel.generated_dir(project_dir)
    if not (root / "CMakePresets.json").exists() \
            or not (generated / appmodel.GENERATED_CMAKE).exists():
        return ("note: nothing generated yet; emitting the deploy layout, licenses, and "
                "manifest; generate the entity build files to compile binaries.")
    need_wasm = "wasm" in client_targets
    if not toolchain.is_complete(resolved, need_wasm=need_wasm):
        # Name what is short, rather than only where to go and ask. The common case is a
        # kit that is installed and missing a module SynQt links, and "toolchain
        # incomplete" on a machine where Qt is plainly installed reads as a bug in the
        # resolver rather than as an answer.
        labels = {"host_qt": "host Qt kit", "wasm_qt": "WebAssembly Qt kit",
                  "emcc": "Emscripten", "cmake": "cmake"}
        pieces = ["host_qt", "cmake"] + (["wasm_qt", "emcc"] if need_wasm else [])
        missing = [f"no {labels[key]}" for key in pieces if not resolved.get(key)]
        missing += [f"Qt6{module} missing from the {labels[kit]}"
                    for kit in ("host_qt", "wasm_qt") if kit in pieces
                    for module in (resolved.get(f"{kit}_missing") or [])]
        return (f"note: toolchain incomplete ({', '.join(missing)}; run 'synqt doctor' for "
                "the commands that provision it); skipped compilation, emitted the deploy "
                "layout and licenses.")
    cmake = resolved["cmake"]
    # Point the host configure at the resolved host Qt kit. The preset carries the
    # provisioned synqt/toolchain path, but a developer with a system Qt (resolved via
    # /opt/Qt or QTDIR) has not populated it. Passing the resolved prefix makes the build
    # work either way without editing the preset.
    # -S names the project itself. Its root CMakeLists.txt includes the generated build,
    # and cmake reads presets from the top-level source directory.
    # The preset carries the build type, the strip switch and SYNQT_DEV_TOOLS, so the CLI
    # and a contributor typing `cmake --preset host-release` configure the same tree. The
    # preset for this profile was written by presets.write() in the caller.
    host_preset = _preset_name("host", profile_name, dev_tools)
    host_configure = [cmake, "-S", str(root), "--preset", host_preset]
    if resolved.get("host_qt"):
        host_configure.append(f"-DCMAKE_PREFIX_PATH={resolved['host_qt']}")
    if edge_url:
        host_configure.append(f"-DSYNQT_EDGE_URL={edge_url}")
    try:
        if host_targets:
            host_dir = project_dir / profiles.build_dir("host", profile_name,
                                                        dev_tools=dev_tools)
            _configure_if_needed(host_configure, host_dir, project_dir, verbose)
            build_command = [cmake, "--build", str(host_dir)]
            for target in host_targets:
                build_command += ["--target", target]
            if verbose:
                build_command.append("--verbose")
            _run(build_command, project_dir, verbose)
        if need_wasm:
            # The WebAssembly client builds through the wasm kit's qt-cmake wrapper (which
            # installs the Emscripten toolchain file). The root CMakeLists guards the
            # service targets behind `if(NOT EMSCRIPTEN)`, so only the client is built.
            qt_cmake = Path(resolved["wasm_qt"]) / "bin" / "qt-cmake"
            # One build directory per kit. Qt-cmake's toolchain choice is cached on the
            # first configure, so a shared directory would silently keep the other kit's.
            wasm_dir = project_dir / clientbuild.wasm_build_dir(config, profile_name,
                                                                 dev_tools)
            # QT_HOST_PATH is passed explicitly, never left to the kit. A cross-compiled Qt has
            # to be told where its host tools (moc, rcc, qmlcachegen) live. The kit bakes in the
            # path from the machine Qt itself was built on (/home/qt/work/install), and aqt
            # rewrites that only when it installs a host-specific kit. The WebAssembly kit is
            # published host-independently (all_os/wasm), so nothing rewrites it and the
            # configure dies on "please set the QT_HOST_PATH cache variable". The host kit
            # is already resolved here, so say so rather than depend on an installer's patching.
            _configure_if_needed([str(qt_cmake), "-S", str(root), "-B", str(wasm_dir),
                                  "-G", "Ninja",
                                  "-DCMAKE_BUILD_TYPE=%s" % profiles.build_type(
                                      profile_name, "wasm", custom_type),
                                  "-DSYNQT_STRIP=%s" % (
                                      "ON" if profiles.strips(profile_name, strip) else "OFF"),
                                  "-DSYNQT_DEV_TOOLS=%s" % ("ON" if dev_tools else "OFF"),
                                  f"-DQT_HOST_PATH={resolved['host_qt']}"],
                                 wasm_dir, project_dir, verbose)
            wasm_build = [cmake, "--build", str(wasm_dir)]
            if verbose:
                wasm_build.append("--verbose")
            _run(wasm_build, project_dir, verbose)
            for target, destination in client_bundle_targets(config).items():
                assemble_bundle(wasm_dir, project_dir / destination, config,
                                project_dir, target)
        return built_note(host_targets, client_targets)
    except subprocess.CalledProcessError as error:
        # A failed compile ends the command. Returning the message as a note, for
        # build() to append to a "Built N entity artifact(s)" summary and print on the
        # way to exit code 0, would make `synqt build` report success, list artifacts it had not
        # produced, and write each one a THIRD-PARTY-LICENSES describing a binary that does not
        # exist.
        raise BuildError(_compile_failure(error, verbose)) from error


# Enough to carry a CMake FATAL_ERROR with its call stack, or a compiler error with the line
# it points at. Past that it is scrollback for something --verbose reports better.
_FAILURE_TAIL_LINES = 20


def _compile_failure(error: subprocess.CalledProcessError, verbose: bool) -> str:
    """Explain a failed build step. Without --verbose the output was captured, so the
    message has to carry it. Cmake's own last line is 'Configuring incomplete, errors
    occurred!', which names nothing, while the FATAL_ERROR that matters is some lines
    above it. Quote the tail rather than one line.

    Both streams, and stdout first. This read stderr alone, which is the wrong half of the
    answer for the step that fails most often. `cmake --build` hands the work to Ninja, and
    Ninja writes its `FAILED:` line and the compiler diagnostics under it to stdout. So a
    compile that broke printed "cmake build failed with no output captured" and named the
    command, which is the one thing the reader already knew. A configure failure does use
    stderr, hence both.
    """
    command = " ".join(str(part) for part in error.cmd)
    if verbose:
        # The output already streamed past. Repeating a slice of it would only bury it.
        return f"cmake build failed (see the output above): {command}"
    captured = "\n".join(part for part in (error.stdout, error.stderr) if part)
    detail = captured.strip().splitlines()
    if not detail:
        return (f"cmake build failed with no output captured, and exit code "
                f"{error.returncode}: {command}")
    tail = "\n".join(f"  {line}" for line in detail[-_FAILURE_TAIL_LINES:])
    return f"cmake build failed: {command}\n{tail}"


def _targets_for(config: Dict[str, Any], client: str) -> Tuple[List[Dict[str, Any]],
                                                               List[str], List[str]]:
    """Resolve the host targets (services, plus each client that builds for desktop) and the
    client targets requested. The browser client compiles through the separate wasm kit.

    Every client entity, not the first. A project may hold a gate and an application, and
    compiling only whichever was declared first leaves the other with nothing while the
    build reports success.
    """
    client_entities = [e for e in appmodel.entities(config) if appmodel.is_client(e)]
    client_targets: List[str] = []
    for entity in client_entities:
        for target in _client_targets(entity, client):
            if target not in client_targets:
                client_targets.append(target)
    host_targets = [e.get("name") for e in config.get("entities", [])
                    if appmodel.is_service(e) and e.get("name")]
    for entity in client_entities:
        if "desktop" in _client_targets(entity, client) and entity.get("name"):
            host_targets.append(entity.get("name"))
    return client_entities, host_targets, client_targets


def compile_incremental(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
                        client: str = "wasm", profile_name: str = "debug",
                        dev_tools: bool = True) -> Tuple[str, List[str], List[str]]:
    """Regenerate the app from the topology and run an incremental cmake build, then
    reinstall the host binaries so a restarted service picks up the new build. Used by
    ``synqt dev``'s watcher (cmake --build is incremental). Returns the compile note plus
    the host and client target lists that were built."""
    root = Path(project_dir).resolve()
    resolved = toolchain.resolve(root, threads=clientbuild.client_threads(config),
                                 add_ons=clientmodules.for_project(config, root))
    appgen.generate(root, config, dev_tools=dev_tools)
    presets.write(root, config, profile_name=profile_name, dev_tools=dev_tools)
    topologywriter.write(root, config)  # the machine topology each service reads at startup
    _, host_targets, client_targets = _targets_for(config, client)
    edge_url = _desktop_edge_url(config) if "desktop" in client_targets else None
    note = _cmake_build(root, resolved, host_targets, client_targets, config=config,
                        edge_url=edge_url, profile_name=profile_name, dev_tools=dev_tools)
    build_dir = root / "build"
    for entity in config.get("entities", []):
        name = entity.get("name")
        if not name:
            continue
        if appmodel.is_client(entity):
            if "desktop" in _client_targets(entity, client):
                _install_binary(build_dir, name,
                                root / appmodel.desktop_output_dir(config, entity)
                                / desktop_platform(), profile_name, dev_tools)
        else:
            _install_binary(build_dir, name, build_dir / name, profile_name, dev_tools)
    return note, host_targets, client_targets


def desktop_platform() -> str:
    """The `build/client-desktop/<platform>/` folder for the host being built on.

    A desktop client is native, so it is always built on the platform it targets (see
    docs/desktop.md, which names these three folders). The name comes from the host rather
    than from config for that reason. There is no cross-building a desktop client here, so a
    configurable value could only ever disagree with what was produced.

    One host-name function, not two. The toolchain resolver needs the same answer to pick
    the host kit directory, and the way both of these went wrong was a second copy of a
    platform assumption drifting from the first.
    """
    return toolchain.host_platform()


def _deployed_note(root: Path, name: str, out: Path, sign: Optional[str]) -> str:
    """The DEPLOY.txt body after `--deploy` has run: what, if anything, is still outstanding.

    Split on whether it was signed, because the two states leave different work. A
    single note covering both would have to hedge, and a hedged note about signing is one
    nobody acts on.
    """
    platform = desktop_platform()
    header = ("This tree was deployed by `synqt build --deploy`: Qt travels with the app and\n"
              "it no longer depends on the kit it was built against. It still expects the\n"
              "host's own system libraries (the C runtime, and the display server's client\n"
              "libraries), which is what every native application on the platform expects.\n\n")
    if sign:
        if platform == "macos":
            return header + (
                f"It was signed as {sign!r}. One step is left before you distribute it:\n\n"
                "    xcrun notarytool submit --wait \\\n"
                f'        --apple-id <you> --team-id <team> "{out / f"{name}.app"}"\n'
                f'    xcrun stapler staple "{out / f"{name}.app"}"\n\n'
                "Notarization needs credentials and a network round trip, so SynQt does not\n"
                "run it. Without it, Gatekeeper still refuses the app on a machine that\n"
                "downloaded it.\n")
        return header + f"It was signed as {sign!r}. Nothing further is required.\n"
    return header + ("It is UNSIGNED.\n\n    " + deploymod.signing_consequence(platform)
                     + "\n\nRe-run with --sign <identity> when you are ready to distribute it.\n")


def _deploy_note(root: Path, name: str, out: Path) -> str:
    """The DEPLOY.txt body. The exact command to run against the artifact this build produced.

    `synqt build` does not run the platform deploy step (docs/desktop.md: signing identities,
    entitlements, notarization and installer format are not a framework's to choose, and a
    half-deployed bundle that looks finished is worse than one that says what is missing). That
    makes this note the whole hand-off, so it names the real path rather than the three tools
    the developer might need. Knowing that `macdeployqt` exists is
    not the missing information. Which folder to run it in is.
    """
    platform = desktop_platform()
    header = ("The platform deploy step is not run by `synqt build` "
              "(https://synqt.org/desktop/); it is "
              "where\nsigning and notarization live. Until it runs, what is here links Qt from "
              "the kit it\nwas built against and runs only on a machine that has that kit.\n\n"
              "For this build, on %s:\n\n" % platform)
    if platform == "macos":
        return header + (
            '    macdeployqt "%s" -qmldir="%s"\n\n'
            "Add -codesign=<identity> to sign, and see `macdeployqt -help` for dmg and\n"
            "hardened-runtime options. The bundle identifier defaults to a placeholder; set\n"
            "-DSYNQT_BUNDLE_ID=<reverse.dns.id> at configure time before you sign.\n"
            % (out / f"{name}.app", root))
    if platform == "windows":
        return header + (
            '    windeployqt --qmldir "%s" "%s"\n' % (root, out / f"{name}.exe"))
    return header + (
        "    synqt build --client desktop --deploy --unsigned\n\n"
        "Linux has no single official tool, so SynQt does this part itself: the QML modules\n"
        "the client imports, the plugin directories it can load, and the transitive closure\n"
        "of Qt libraries all of that needs, beside the binary, with a launcher that sets\n"
        "LD_LIBRARY_PATH, QML_IMPORT_PATH and QT_PLUGIN_PATH. There is nothing to sign; on\n"
        "Linux --unsigned is the normal state. For one distributable file, wrap the result\n"
        "with linuxdeploy or an AppImage recipe. The binary is %s.\n" % (out / name))


def _install_binary(build_dir: Path, entity_name: str, dest: Path,
                    profile_name: str = "debug", dev_tools: bool = False) -> bool:
    """Copy a compiled host binary into its deploy directory so `synqt serve` finds it
    alongside its THIRD-PARTY-LICENSES. Returns True when a binary was installed.

    The suffix is resolved rather than assumed (run.host_binary): Windows links `<name>.exe`, so
    looking only for the bare name there finds nothing, and this returns False for a binary that
    built perfectly well. A deploy directory that is silently missing its executable.

    On macOS the desktop client is an .app bundle, so what gets installed is a directory tree
    and not a file. Copying it with copy2 raised IsADirectoryError. Copying only the executable
    inside it would have been worse, silently producing a deploy folder holding something that
    is no longer an app.
    """
    compiled = run.host_artifact(build_dir.parent, entity_name, profile_name, dev_tools)
    if compiled is None:
        return False
    dest.mkdir(parents=True, exist_ok=True)
    target = dest / compiled.name
    if compiled.is_dir():
        shutil.rmtree(target, ignore_errors=True)  # a stale bundle would otherwise merge
        shutil.copytree(compiled, target, symlinks=True)
    else:
        shutil.copy2(compiled, target)
    return True


# Everything on the first-visit critical path that compresses well. The wasm dominates,
# but the Emscripten glue .js is the second-largest asset and was previously shipped raw.
# The .gz/.br variants themselves match none of these, so a second pass is a no-op rather
# than a way to produce client.wasm.gz.gz.
_COMPRESSIBLE = ("*.wasm", "*.js", "*.html", "*.json", "*.svg")


def _is_current(variant: Path, source_mtime: int) -> bool:
    """Whether a compressed variant was written from the asset as it stands now."""
    try:
        return variant.stat().st_mtime_ns >= source_mtime
    except OSError:
        return False


def precompress(client_dir: Path) -> int:
    """Brotli + gzip every compressible bundle asset so the edge can serve the smaller
    copy. The edge picks per request from Accept-Encoding. These are additions beside the
    original, never replacements. Returns how many assets were compressed here.

    An asset whose variants are already newer than it is left alone, because recompressing
    an unchanged bundle is the most expensive thing a no-op build can do: Brotli over a
    30 MB `.wasm` is tens of seconds of one core, and it dominated the no-op and edit
    rebuild figures in benchmarks/results/buildtime-*.json (38 s of a 38.7 s no-op, while
    the compiler did nothing). The bundle is assembled with `shutil.copy2`, which carries
    the compiled artifact's timestamp across, so an asset the build did not rebuild keeps
    the mtime its variants were made from."""
    count = 0
    for pattern in _COMPRESSIBLE:
        for asset in sorted(Path(client_dir).glob(pattern)):
            source_mtime = asset.stat().st_mtime_ns
            gzipped = asset.with_name(asset.name + ".gz")
            brotlied = asset.with_name(asset.name + ".br")
            try:
                import brotli
            except ImportError:
                brotli = None
            needs_gzip = not _is_current(gzipped, source_mtime)
            needs_brotli = brotli is not None and not _is_current(brotlied, source_mtime)
            if not needs_gzip and not needs_brotli:
                continue
            data = asset.read_bytes()
            if needs_gzip:
                gzipped.write_bytes(gzip.compress(data, 9))
            if needs_brotli:
                brotlied.write_bytes(brotli.compress(data))
            count += 1
    return count


def write_process_manifest(config: Dict[str, Any], build_dir: Path) -> Path:
    """A dependency-ordered start plan: owners before consumers, only the edge public."""
    order = run.startup_order(config)
    edges = {e.get("name") for e in config.get("entities", [])
             if appmodel.is_edge(e)}
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
    """The entities this build acts on. All of them, or the one `--entity` names.

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


def volatile_store_notices(entities: List[Dict[str, Any]]) -> List[str]:
    """Which of these entities keep everything they are given in memory.

    The document type's embedded default is an in-process store. It holds what it is
    given for as long as the process lives and not one moment longer, and it has no
    bound, because a store that forgets is not a store. That is the right default for
    getting started and the wrong one to find out about after a deploy, so a build that
    produced such an entity says so, once, beside the licence reminders and on the same
    terms as them. Only for an artifact this build made.
    """
    notices: List[str] = []
    for entity in entities:
        if appmodel.entity_type(entity) != "document":
            continue
        provider = (entity.get("provider") or {}).get("name") or "memory"
        if provider != "memory":
            continue
        notices.append(
            f"Note: '{entity.get('name')}' keeps its documents in memory, which is the "
            "document type's embedded default: they are gone when the process stops, and "
            "nothing bounds how many it holds. Select the mongodb provider for storage "
            "that outlives a restart. See https://synqt.org/providers/.")
    return notices


def build(project_dir: os.PathLike[str] | str, *, profile_name: str = "debug",
          custom_type: str = "", strip: bool = False, dev_tools: bool = False,
          client: str = "wasm", qt_license_mode: str = "open_source",
          entity: Optional[str] = None, threads: Optional[str] = None,
          verbose: bool = False, profile: Optional[str] = None,
          deploy: bool = False, sign: Optional[str] = None) -> str:
    """Build every selected entity for one profile.

    `profile_name` is the build profile (`debug`, `release` or `custom`) and `profile` is
    the configuration layer that `--profile NAME` selects. They are unrelated and the names
    are unfortunately close. The first decides how this is compiled and the second decides
    what is compiled. `dev_tools` is never true from `synqt build`, whatever the profile:
    only `synqt dev` builds a tree that carries development-only code.
    """
    # Resolve to an absolute path. The cmake invocations below run with cwd set to the
    # project dir, so a relative --project-dir would otherwise be joined against itself.
    root = Path(project_dir).resolve()
    config = clientbuild.with_threads(load_config(root, profile), threads)
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)
    resolved = toolchain.resolve(root, threads=clientbuild.client_threads(config),
                                 add_ons=clientmodules.for_project(config, root))
    selected = _selected_entities(config, entity)

    # Regenerate the app from the current topology so a connect-point change is reflected
    # in the CMakeLists, the CMakePresets, and the per-entity main before compiling. This
    # keeps `synqt build` self-sufficient on any project (a docs example, a hand-authored
    # tree), not only one scaffolded by `synqt new`.
    appgen.generate(root, config, dev_tools=dev_tools)
    presets.write(root, config, profile_name=profile_name, custom_type=custom_type,
                  strip=strip, dev_tools=dev_tools)
    topologywriter.write(root, config)  # the machine topology each service reads at startup

    # Only among the selected entities. `--entity web` must not compile the client too.
    # Every selected client, not the first one: a project may hold a gate and an
    # application, and building only whichever was declared first would leave the other
    # with no bundle while reporting success.
    client_entities = [e for e in selected if appmodel.is_client(e)]
    client_targets: List[str] = []
    for entity in client_entities:
        for target in _client_targets(entity, client):
            if target not in client_targets:
                client_targets.append(target)

    # Host targets. Every service entity, plus the client only when a desktop build is
    # requested (the browser client compiles through the separate wasm kit).
    host_targets = [e.get("name") for e in selected if appmodel.is_service(e)]
    for client_entity in client_entities:
        if "desktop" in _client_targets(client_entity, client):
            host_targets.append(client_entity.get("name"))
    edge_url = _desktop_edge_url(config) if "desktop" in client_targets else None
    compile_note = _cmake_build(root, resolved, host_targets, client_targets,
                                config=config, edge_url=edge_url, verbose=verbose,
                                profile_name=profile_name, custom_type=custom_type,
                                strip=strip, dev_tools=dev_tools)

    produced: List[str] = []
    deploy_notes: List[str] = []
    for entity in selected:
        name = entity.get("name")
        if appmodel.is_client(entity):
            for target in _client_targets(entity, client):
                folder = (appmodel.bundle_output_dir(config, entity) if target == "wasm"
                          else appmodel.desktop_output_dir(config, entity))
                # Both are project-root relative ("build/client"), and `out` is built from
                # the project root so the historic path stays byte-identical.
                out = build_dir.parent / folder
                if target == "desktop":
                    # The host's own folder (windows/, macos/, linux/ per docs/desktop.md). A
                    # desktop client is native, so the only one this build can fill is this
                    # host's. The others come from that platform's own run of the same command.
                    out = out / desktop_platform()
                out.mkdir(parents=True, exist_ok=True)
                (out / "THIRD-PARTY-LICENSES").write_text(
                    licenses.generate(entity, target=target,
                                      qt_license_mode=qt_license_mode, config=config,
                                      project_dir=root))
                # The desktop client compiles on the host. Place it beside its licenses.
                # Installed before the note is written, so the note can name the artifact that
                # is there rather than the one this build expected to produce.
                if target == "desktop":
                    _install_binary(build_dir, name, out, profile_name, dev_tools)
                    if deploy:
                        # Asked for explicitly, so a failure here is a failed build rather than
                        # a warning. The developer said they wanted a deployed tree, and one
                        # that silently is not deployed is the "looks finished" outcome the
                        # default position exists to avoid.
                        deploy_notes.append(
                            deploymod.deploy_client(root, name, out, resolved,
                                                    desktop_platform(), sign=sign))
                        (out.parent / "DEPLOY.txt").write_text(
                            _deployed_note(root, name, out, sign))
                    else:
                        (out.parent / "DEPLOY.txt").write_text(_deploy_note(root, name, out))
                produced.append(f"{folder}/ ({target})")
        else:
            out = build_dir / name
            out.mkdir(parents=True, exist_ok=True)
            (out / "THIRD-PARTY-LICENSES").write_text(
                licenses.generate(entity, qt_license_mode=qt_license_mode, config=config))
            # into build/<entity>/, which is what `synqt serve` launches: the deploy
            # layout holds whichever profile was built last. It is the artifact
            # tree, and an artifact does not carry a profile in its path.
            _install_binary(build_dir, name, out, profile_name, dev_tools)
            produced.append(f"build/{name}/")

    # Only when this build produced the bundle. With --entity web the client dir may still
    # hold an older bundle, and recompressing it would report work this build did not do.
    bundle_dirs = [build_dir.parent / destination
                   for destination in client_bundle_targets(config).values()]
    built_wasm_client = "wasm" in client_targets and any(d.exists() for d in bundle_dirs)
    compressed = sum(precompress(directory) for directory in bundle_dirs
                     if directory.exists()) if built_wasm_client else 0
    write_process_manifest(config, build_dir)

    # The profile, named rather than reduced to two words. A line that reads "release"
    # or "debug" from a flag that reaches nothing else is the only place the profile
    # exists, and it is as often wrong as right.
    described = custom_type if profile_name == "custom" else profile_name
    if dev_tools:
        described += ", with development code"
    summary = [f"Built {len(produced)} entity artifact(s) ({described}):"]
    summary += [f"  - {item}" for item in produced]
    summary.append(f"  {compile_note}")
    if compressed:
        summary.append(f"  precompressed {compressed} bundle file(s) (Brotli + gzip).")
    elif built_wasm_client:
        summary.append("  bundle already precompressed; nothing changed to redo.")
    summary.append("  wrote build/process-manifest.json (owners start before consumers).")
    summary += [f"  {note}" for note in deploy_notes]

    # Each licence reminder belongs to an artifact this build produced. With
    # --entity database, warning about a client that was not built teaches the reader to
    # skim past the warning, which is how the one that matters gets missed.
    if qt_license_mode == "open_source":
        notices: List[str] = []
        if client_targets:
            notices.append(licenses.CLIENT_GPL_WARNING)
        if any(appmodel.is_edge(e) for e in selected):
            notices.append("Note: distributing the edge binary triggers GPLv3 (Qt HTTP "
                           "Server / Network Authorization). See https://synqt.org/licensing/.")
        if notices:
            summary += [""] + notices
    # Not a licence matter, so it is outside the block above and is said whichever Qt
    # licence this build was made under.
    volatile = volatile_store_notices(selected)
    if volatile:
        summary += [""] + volatile
    return "\n".join(summary)
