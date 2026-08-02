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

from . import (appgen, clientbuild, clientcache, clientshell, config as configmod,
               deploy as deploymod, licenses, manifest, presets, run, toolchain,
               topologywriter, writer)


class BuildError(Exception):
    """A build error surfaced to the CLI (no traceback for the user)."""


def load_config(project_dir: os.PathLike[str] | str,
                profile: Optional[str] = None) -> Dict[str, Any]:
    """The effective configuration for a build: synqt.yaml under its profile and the
    environment layer (see `config.resolve`)."""
    return configmod.load(project_dir, profile=profile, required=True)


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
        clientshell.render_client_shell(f"{target}.js", config, project_dir))
    writer.write_if_changed(client_dir / "synqt-boot.js",
                            clientshell.render_boot_js(target, config))
    extra = 2
    # Written before the manifest so the worker appears in the manifest's file list and
    # therefore precaches itself along with the rest of the shell.
    if clientcache.uses_service_worker(config):
        writer.write_if_changed(client_dir / "synqt-sw.js",
                                clientshell.render_service_worker_js())
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


def _preset_generator(project_dir: Path, preset: str) -> Optional[str]:
    """The generator a configure preset names, following `inherits`. None when the preset
    leaves it to CMake's per-platform default, in which case there is nothing to compare."""
    presets_file = project_dir / "CMakePresets.json"
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
        if entry.get("generator"):
            return str(entry["generator"])
        inherits = entry.get("inherits")
        preset = inherits[0] if isinstance(inherits, list) and inherits else inherits
    return None


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


def _clear_incompatible_cache(configure: List[str], build_dir: Path,
                              project_dir: Path) -> Optional[str]:
    """Delete a CMake cache that was made by a different generator than the preset asks
    for, so the configure below can succeed. Returns a line to report, or None.

    CMake refuses outright to reconfigure such a directory ("Does not match the generator
    used previously"), and it is the build directory synqt owns and would have created
    itself, so the fix is synqt's to apply rather than an error to hand back. It is not a
    hypothetical: the host preset moved to Ninja so the host build has the same shape on
    Windows as elsewhere, which left every project configured before that change unable
    to build until someone deleted a directory cmake named only indirectly.

    Only the cache and CMakeFiles go: build outputs are left alone, so this costs a
    reconfigure and a rebuild of what changed, not the whole tree.
    """
    if "--preset" not in configure:
        return None
    wanted = _preset_generator(project_dir, configure[configure.index("--preset") + 1])
    existing = _cached_generator(build_dir)
    if not wanted or not existing or wanted == existing:
        return None
    shutil.rmtree(build_dir / "CMakeFiles", ignore_errors=True)
    try:
        (build_dir / "CMakeCache.txt").unlink()
    except OSError:
        return None
    return (f"note: {build_dir} was configured with {existing} and the preset now asks "
            f"for {wanted}; reconfiguring it from scratch.")


def _configure_if_needed(configure: List[str], build_dir: Path, project_dir: Path,
                         verbose: bool) -> bool:
    """Configure the build directory, unless it is already configured with this exact
    command. Returns whether cmake was run.

    Skipping is safe because it is not a shortcut around a stale configure. The generator
    now writes only files whose content changed (synqt.writer), so an unchanged
    `CMakeLists.txt` keeps its modification time, and the generator itself is what the
    build system watches: ninja re-runs cmake on its own the moment one of those files
    does change. What is left to skip is the case where nothing changed at all, where
    cmake re-derives an identical build graph.

    The stamp records the command, not just the fact of configuring, so a build with a
    different Qt kit or a different `-DSYNQT_EDGE_URL` configures again rather than
    silently inheriting the cache from the last one. It records `CMakePresets.json` with
    it, because that file is read when cmake is invoked and is not one of the inputs the
    generated build graph watches: ninja re-runs cmake for a changed `CMakeLists.txt` and
    would not notice a preset that moved the build type or added a cache variable.
    """
    stamp = build_dir / ".synqt-configure"
    presets_file = project_dir / "CMakePresets.json"
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
            _configure_if_needed(host_configure, project_dir / "build" / "host",
                                 project_dir, verbose)
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
            # rewrites that only when it installs a host-specific kit; the WebAssembly kit is
            # published host-independently (all_os/wasm), so nothing rewrites it and the
            # configure dies on "please set the QT_HOST_PATH cache variable". We already
            # resolved the host kit, so say so rather than depend on an installer's patching.
            _configure_if_needed([str(qt_cmake), "-S", str(project_dir), "-B", str(wasm_dir),
                                  "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                  f"-DQT_HOST_PATH={resolved['host_qt']}"],
                                 wasm_dir, project_dir, verbose)
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


def _deployed_note(root: Path, name: str, out: Path, sign: Optional[str]) -> str:
    """The DEPLOY.txt body after `--deploy` has run: what, if anything, is still outstanding.

    Split on whether it was signed, because the two states leave genuinely different work. A
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
    """The DEPLOY.txt body: the exact command to run against the artifact this build produced.

    `synqt build` does not run the platform deploy step (docs/desktop.md: signing identities,
    entitlements, notarization and installer format are not a framework's to choose, and a
    half-deployed bundle that looks finished is worse than one that says what is missing). That
    makes this note the whole hand-off, so it names the real path rather than the three tools
    the developer might need, which is what it used to do: knowing that `macdeployqt` exists is
    not the missing information, and the previous text did not even say which folder to run it in.
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


def _install_binary(build_dir: Path, entity_name: str, dest: Path) -> bool:
    """Copy a compiled host binary into its deploy directory so `synqt serve` finds it
    alongside its THIRD-PARTY-LICENSES. Returns True when a binary was installed.

    The suffix is resolved rather than assumed (run.host_binary): Windows links `<name>.exe`, so
    looking only for the bare name there finds nothing, and this returns False for a binary that
    built perfectly well: a deploy directory that is silently missing its executable.

    On macOS the desktop client is an .app bundle, so what gets installed is a directory tree
    and not a file. Copying it with copy2 raised IsADirectoryError; copying only the executable
    inside it would have been worse, silently producing a deploy folder holding something that
    is no longer an app.
    """
    compiled = run.host_artifact(build_dir.parent, entity_name)
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
    copy. The edge picks per request from Accept-Encoding; these are additions beside the
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
          verbose: bool = False, profile: Optional[str] = None,
          deploy: bool = False, sign: Optional[str] = None) -> str:
    # Resolve to an absolute path: the cmake invocations below run with cwd set to the
    # project dir, so a relative --project-dir would otherwise be joined against itself.
    root = Path(project_dir).resolve()
    config = clientbuild.with_threads(load_config(root, profile), threads)
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
    deploy_notes: List[str] = []
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
                (out / "THIRD-PARTY-LICENSES").write_text(
                    licenses.generate(entity, target=target, qt_license_mode=qt_license_mode))
                # The desktop client compiles on the host; place it beside its licenses.
                # Installed before the note is written, so the note can name the artifact that
                # is actually there rather than the one this build expected to produce.
                if target == "desktop":
                    _install_binary(build_dir, name, out)
                    if deploy:
                        # Asked for explicitly, so a failure here is a failed build rather than
                        # a warning: the developer said they wanted a deployed tree, and one
                        # that silently is not deployed is the "looks finished" outcome the
                        # default position exists to avoid.
                        deploy_notes.append(
                            deploymod.deploy_client(root, name, out, resolved,
                                                    desktop_platform(), sign=sign))
                        (out.parent / "DEPLOY.txt").write_text(
                            _deployed_note(root, name, out, sign))
                    else:
                        (out.parent / "DEPLOY.txt").write_text(_deploy_note(root, name, out))
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
    elif built_wasm_client:
        summary.append("  bundle already precompressed; nothing changed to redo.")
    summary.append("  wrote build/process-manifest.json (owners start before consumers).")
    summary += [f"  {note}" for note in deploy_notes]

    # Each licence reminder belongs to an artifact this build actually produced. With
    # --entity database, warning about a client that was not built teaches the reader to
    # skim past the warning, which is how the one that matters gets missed.
    if qt_license_mode == "open_source":
        notices: List[str] = []
        if client_targets:
            notices.append(licenses.CLIENT_GPL_WARNING)
        if any(e.get("capability") == "web_edge" or e.get("web_edge") for e in selected):
            notices.append("Note: distributing the edge binary triggers GPLv3 (Qt HTTP "
                           "Server / Network Authorization). See https://synqt.org/licensing/.")
        if notices:
            summary += [""] + notices
    return "\n".join(summary)
