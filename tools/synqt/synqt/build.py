# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt build``: compile every entity through the pinned toolchain, emit one deployable
directory per entity with an accurate THIRD-PARTY-LICENSES, precompress the client bundle,
and write a dependency-ordered process manifest.

Compilation runs through the generated CMake presets (the host kit for services and the
desktop client, the WebAssembly kit for the browser client).
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
    """The effective configuration for a build: synqt.yaml with its profile and environment
    layers (see `config.resolve`).
    """
    return configmod.load(project_dir, profile=profile, required=True)


def _client_targets(entity: Dict[str, Any], requested: str) -> List[str]:
    declared = appmodel.client_targets(entity)
    if requested == "all":
        return declared
    return [requested] if requested in declared else []


def _wasm_runtime_files(wasm_dir: Path, target: Optional[str] = None) -> List[Path]:
    """The Emscripten runtime and assets to serve (.js, .wasm, .svg). Qt's .html is not
    shipped: SynQt writes its own CSP-clean shell.

    With `target`, only that target's artifacts plus the shared files (qtloader.js, assets).
    One Emscripten build directory holds every client's `<target>.js` and `<target>.wasm`.
    """
    shared = {"qtloader.js"}
    wanted: List[Path] = []
    for pattern in ("*.js", "*.wasm", "*.svg"):
        # qtlogo.svg belongs to Qt's stock template, which SynQt replaces.
        for path in sorted(wasm_dir.glob(pattern)):
            if path.name == "qtlogo.svg":
                continue
            if (target is not None and path.suffix in (".js", ".wasm")
                    and path.stem != target and path.name not in shared):
                continue
            wanted.append(path)
    return wanted


def client_bundle_targets(config: Dict[str, Any]) -> Dict[str, str]:
    """Every client entity to assemble a bundle for, mapped to its output directory.

    Keyed by entity name, which is also the CMake target. A desktop-only client is absent.
    """
    return {str(entity.get("name") or ""): appmodel.bundle_output_dir(config, entity)
            for entity in appmodel.entities(config)
            if appmodel.is_client(entity) and "wasm" in appmodel.client_targets(entity)}


def assemble_bundle(wasm_dir: Path, client_dir: Path, config: Dict[str, Any],
                    project_dir: Path, target: Optional[str] = None) -> int:
    """Assemble the served bundle: copy the WASM runtime and assets, then write SynQt's
    CSP-clean index.html and synqt-boot.js (Qt's template boots from an inline handler the
    CSP blocks). Returns the file count.
    """
    client_dir.mkdir(parents=True, exist_ok=True)
    runtime = _wasm_runtime_files(wasm_dir, target)
    # The app runtime is <target>.js, the loader qtloader.js, the entry symbol
    # window.<target>_entry.
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
    # Written before the manifest, so the worker precaches itself.
    if clientcache.uses_service_worker(config):
        writer.write_if_changed(client_dir / "synqt-sw.js",
                                clientshell.render_service_worker_js(target))
        extra += 1

    # Written last. Precompression has not run, so .br/.gz are not listed.
    if app_js is not None and (client_dir / f"{target}.wasm").is_file():
        manifest.write(client_dir, f"{target}.wasm")
        return count + extra + 1
    return count + extra


def _desktop_edge_url(config: Dict[str, Any]) -> Optional[str]:
    """The edge URL a desktop client connects to (build.desktop.edge_url), compiled in as
    SYNQT_EDGE_URL. None when unset, keeping the CMake default.
    """
    desktop = ((config.get("build") or {}).get("desktop") or {})
    url = desktop.get("edge_url")
    return url if isinstance(url, str) and url else None


def _run(command: List[str], cwd: Path, verbose: bool) -> None:
    """Run a build step. Quiet by default; with --verbose the command is echoed and its output
    streams through.
    """
    if verbose:
        print("  $ " + " ".join(str(part) for part in command), flush=True)
        subprocess.run(command, cwd=cwd, check=True)
        return
    subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)


def built_note(host_targets: List[str], client_targets: List[str]) -> str:
    """Name what was compiled. With --entity this note is what says the build was partial."""
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
    leaves it to CMake.
    """
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


# Where a configured tree records its Qt. CMake does not follow a change of kit.
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
    """Every Qt kit this configure command names, resolved: the wasm qt-cmake wrapper,
    QT_HOST_PATH or CMAKE_PREFIX_PATH, and the preset prefix path and toolchain file.
    Resolved through the synqt/toolchain links.
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
    """Delete a CMake cache this configure cannot reuse. Returns a line to report, or None.

    Two caches cannot be reused: one made by another generator ("Does not match the
    generator used previously"), and one made against another Qt kit (its toolchain file and
    Qt6_DIR are cached). Only the cache and CMakeFiles are deleted; build outputs stay.
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
    """Configure the build directory unless it is already configured with this exact command.
    Returns whether cmake ran.

    The generator writes only changed files, and Ninja reruns cmake when one changes. The
    stamp records the command (so a different kit or `-DSYNQT_EDGE_URL` reconfigures) and
    `CMakePresets.json`, which Ninja does not watch.
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
    # Written only after a successful configure.
    build_dir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(command_line, encoding="utf-8")
    return True


def _preset_name(environment: str, profile_name: str, dev_tools: bool) -> str:
    """Which generated configure preset this build uses. `host` and `wasm` are debug. The
    development tree has its own preset, so SYNQT_DEV_TOOLS=ON is always named.
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
    """Compile the host targets (services and an optional desktop client) and, when requested,
    the browser client through the Emscripten Qt kit. A desktop build bakes in
    build.desktop.edge_url as SYNQT_EDGE_URL.
    """
    root = Path(project_dir)
    generated = appmodel.generated_dir(project_dir)
    if not (root / "CMakePresets.json").exists() \
            or not (generated / appmodel.GENERATED_CMAKE).exists():
        return ("note: nothing generated yet; emitting the deploy layout, licenses, and "
                "manifest; generate the entity build files to compile binaries.")
    need_wasm = "wasm" in client_targets
    if not toolchain.is_complete(resolved, need_wasm=need_wasm):
        # Name what is missing: usually an installed kit short of a module SynQt links.
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
    # Point the host configure at the resolved host kit, which also covers a system Qt
    # (/opt/Qt, QTDIR) the synqt/toolchain links do not name. -S is the project, whose root
    # CMakeLists includes the generated build. The preset (from presets.write()) carries the
    # build type, the strip switch and SYNQT_DEV_TOOLS, so `cmake --preset host-release`
    # configures the same tree.
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
            # The browser client builds through the wasm qt-cmake wrapper. The root
            # CMakeLists skips the service targets under EMSCRIPTEN.
            qt_cmake = Path(resolved["wasm_qt"]) / "bin" / "qt-cmake"
            # One build directory per kit: qt-cmake caches its toolchain on first configure.
            wasm_dir = project_dir / clientbuild.wasm_build_dir(config, profile_name,
                                                                 dev_tools)
            # QT_HOST_PATH is passed explicitly. The WebAssembly kit is published
            # host-independently (all_os/wasm), so aqt never rewrites its baked-in host
            # path.
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
        # A failed compile ends the command, so no summary or license file is written for a
        # binary that does not exist.
        raise BuildError(_compile_failure(error, verbose)) from error


# Enough for a CMake FATAL_ERROR with its call stack, or a compiler error with its line.
_FAILURE_TAIL_LINES = 20


def _compile_failure(error: subprocess.CalledProcessError, verbose: bool) -> str:
    """Explain a failed build step, quoting the tail of the captured output.

    Both streams, stdout first: Ninja writes `FAILED:` and the compiler diagnostics to
    stdout, a configure failure writes to stderr.
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
    """Resolve the host targets (services, plus each client with a desktop target) and the
    requested client targets. The browser client compiles through the wasm kit. Every client
    entity is included.
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
    """Regenerate the app, run an incremental cmake build, and reinstall the host binaries so a
    restarted service picks them up. Used by the ``synqt dev`` watcher. Returns the compile
    note and the host and client targets built.
    """
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

    A desktop client is always built on its target platform (docs/desktop.md), so the name
    comes from the host. The toolchain resolver uses the same function.
    """
    return toolchain.host_platform()


def _deployed_note(root: Path, name: str, out: Path, sign: Optional[str]) -> str:
    """The DEPLOY.txt body after `--deploy` has run: what is still outstanding, depending on
    whether it was signed.
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
    """The DEPLOY.txt body: the exact command to run against the artifact this build produced.

    `synqt build` does not run the platform deploy step: signing, entitlements, notarization
    and installer format are the project's choice (docs/desktop.md).
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
    """Copy a compiled host binary into its deploy directory, beside its THIRD-PARTY-LICENSES.
    Returns True when a binary was installed.

    The suffix is resolved (run.host_binary), since Windows links `<name>.exe`. On macOS the
    desktop client is an .app bundle, so the whole directory is copied.
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


# First-visit assets that compress well. The .gz/.br variants match none of these.
_COMPRESSIBLE = ("*.wasm", "*.js", "*.html", "*.json", "*.svg")


def _is_current(variant: Path, source_mtime: int) -> bool:
    """Whether a compressed variant was written from the asset as it stands now."""
    try:
        return variant.stat().st_mtime_ns >= source_mtime
    except OSError:
        return False


def precompress(client_dir: Path) -> int:
    """Brotli and gzip every compressible bundle asset, beside the original. The edge picks per
    request from Accept-Encoding. Returns how many assets were compressed.

    An asset whose variants are newer is skipped: Brotli over a large `.wasm` takes tens of
    seconds. The bundle is assembled with `shutil.copy2`, which keeps the timestamp of an
    artifact that was not rebuilt.
    """
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
    """The entities this build acts on: all of them, or the one `--entity` names. An unknown
    name is an error.
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
    """Which of these entities keep everything in memory.

    The document type's embedded default is an unbounded in-process store that forgets
    everything on exit. A build that produced one says so once, beside the licence
    reminders.
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

    `profile_name` is the build profile (`debug`, `release` or `custom`): how it is
    compiled. `profile` is the configuration layer `--profile NAME` selects: what is
    compiled. `dev_tools` is never true from `synqt build`.
    """
    # Absolute, because cmake runs with the project dir as cwd.
    root = Path(project_dir).resolve()
    config = clientbuild.with_threads(load_config(root, profile), threads)
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)
    resolved = toolchain.resolve(root, threads=clientbuild.client_threads(config),
                                 add_ons=clientmodules.for_project(config, root))
    selected = _selected_entities(config, entity)

    # Regenerate the app from the topology first, so `synqt build` works on any project, not
    # only one made by `synqt new`.
    appgen.generate(root, config, dev_tools=dev_tools)
    presets.write(root, config, profile_name=profile_name, custom_type=custom_type,
                  strip=strip, dev_tools=dev_tools)
    topologywriter.write(root, config)  # the machine topology each service reads at startup

    # Only among the selected entities, and every selected client.
    client_entities = [e for e in selected if appmodel.is_client(e)]
    client_targets: List[str] = []
    for entity in client_entities:
        for target in _client_targets(entity, client):
            if target not in client_targets:
                client_targets.append(target)

    # Host targets: every service, plus a client with a desktop target.
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
                # the project root.
                out = build_dir.parent / folder
                if target == "desktop":
                    # This host's folder (windows/, macos/, linux/, docs/desktop.md). Other
                    # platforms come from their own run.
                    out = out / desktop_platform()
                out.mkdir(parents=True, exist_ok=True)
                (out / "THIRD-PARTY-LICENSES").write_text(
                    licenses.generate(entity, target=target,
                                      qt_license_mode=qt_license_mode, config=config,
                                      project_dir=root))
                # Install the desktop client beside its licenses, before the note that names
                # it.
                if target == "desktop":
                    _install_binary(build_dir, name, out, profile_name, dev_tools)
                    if deploy:
                        # --deploy was asked for, so a failure here fails the build.
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
                licenses.generate(entity, qt_license_mode=qt_license_mode, config=config,
                                  linked=licenses.provider_libraries(
                                      root / profiles.build_dir("host", profile_name,
                                                                dev_tools=dev_tools))))
            # into build/<entity>/, which `synqt serve` launches. It holds the last profile
            # built.
            _install_binary(build_dir, name, out, profile_name, dev_tools)
            produced.append(f"build/{name}/")

    # Only when this build produced the bundle.
    bundle_dirs = [build_dir.parent / destination
                   for destination in client_bundle_targets(config).values()]
    built_wasm_client = "wasm" in client_targets and any(d.exists() for d in bundle_dirs)
    compressed = sum(precompress(directory) for directory in bundle_dirs
                     if directory.exists()) if built_wasm_client else 0
    write_process_manifest(config, build_dir)

    # Name the profile.
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

    # Licence reminders only for artifacts this build produced.
    if qt_license_mode == "open_source":
        notices: List[str] = []
        if client_targets:
            notices.append(licenses.CLIENT_GPL_WARNING)
        if any(appmodel.is_edge(e) for e in selected):
            notices.append("Note: distributing the edge binary triggers GPLv3 (Qt HTTP "
                           "Server / Network Authorization). See https://synqt.org/licensing/.")
        if notices:
            summary += [""] + notices
    # Not a licence matter, so outside the block above.
    volatile = volatile_store_notices(selected)
    if volatile:
        summary += [""] + volatile
    return "\n".join(summary)
