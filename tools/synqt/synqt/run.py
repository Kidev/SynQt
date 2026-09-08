# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt serve`` and ``synqt test``: run the built entities and the test suite.

serve starts each service entity in dependency order (owners before consumers, so a
consumer's owner is up before the consumer tries to acquire it), with only the web edge
on a public interface. test builds and runs the project's CTest suite.
"""

from __future__ import annotations

import os
import secrets
import shutil
import subprocess
import time
import webbrowser
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import yaml

from . import appmodel
from . import (clientshell, cmakegen, config as configmod, devidentities, mesh, profiles,
               toolchain)


def launch_env(root: Path) -> Dict[str, str]:
    """The environment an entity binary is launched with.

    On Windows, the Qt kit's bin directory is added to PATH: there is no RPATH, so an entity
    cannot find Qt6Core.dll otherwise. Every host gets QT_QPA_PLATFORM=offscreen unless one
    is set, because an edge runs a QGuiApplication and aborts on a Linux host with no
    display.
    """
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    # toolchain.host_platform() is the one host check; build.desktop_platform() uses it too.
    if toolchain.host_platform() != "windows":
        return env
    host_qt = resolved_host_qt(root)
    if host_qt:
        env["PATH"] = str(Path(host_qt) / "bin") + os.pathsep + env.get("PATH", "")
    return env


def resolved_host_qt(root: Path) -> Optional[str]:
    """The resolved host Qt kit for this project, or None. Separate so launch_env can be tested
    without a toolchain.
    """
    return toolchain.resolve(root).get("host_qt")


def _executable(directory: Path, name: str) -> Optional[Path]:
    """The executable called `name` in `directory`, or None.

    The suffix is resolved, since only Windows adds .exe. The macOS desktop client is an
    .app bundle (cmakegen sets MACOSX_BUNDLE for macdeployqt, docs/desktop.md), and the
    executable inside it is returned.
    """
    for suffix in ("", ".exe"):
        candidate = directory / f"{name}{suffix}"
        if candidate.is_file():
            return candidate
    inner = directory / f"{name}.app" / "Contents" / "MacOS" / name
    if inner.is_file():
        return inner
    return None


def host_build_dir(root: Path, profile_name: str = "debug",
                   dev_tools: bool = False) -> Path:
    """Where one profile's host binaries are. Each profile builds into its own directory (see
    profiles.build_dir).
    """
    return root / profiles.build_dir("host", profile_name, dev_tools=dev_tools)


def host_binary(root: Path, name: str, profile_name: str = "debug",
                dev_tools: bool = False) -> Optional[Path]:
    """The compiled host executable for one entity, or None if that profile never built it."""
    return _executable(host_build_dir(root, profile_name, dev_tools), name)


def built_profiles(root: Path, name: str) -> List[str]:
    """Which profiles have this entity built, so a miss can name them."""
    found = []
    for candidate in profiles.PROFILES:
        for dev in (False, True):
            if host_binary(root, name, candidate, dev):
                found.append(candidate + ("-dev" if dev else ""))
    return found


def host_artifact(root: Path, name: str, profile_name: str = "debug",
                  dev_tools: bool = False) -> Optional[Path]:
    """What a deploy step copies for one entity: the .app bundle on macOS, otherwise the
    executable. Differs from host_binary() on macOS, where the bundle is what can be signed
    and what macdeployqt takes.
    """
    bundle = host_build_dir(root, profile_name, dev_tools) / f"{name}.app"
    if bundle.is_dir():
        return bundle
    return host_binary(root, name, profile_name, dev_tools)


def _deployed_binary(root: Path, name: str) -> Optional[Path]:
    """The entity executable in its deploy directory (installed by `synqt build`, launched by
    `synqt serve`), or None.
    """
    return _executable(root / "build" / name, name)


def _edge_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return next((e for e in appmodel.entities(config) if appmodel.is_edge(e)), None)


def startup_order(config: Dict[str, Any]) -> List[str]:
    """Service entities ordered so every owner starts before its consumers."""
    # Including the links `identity.provider_entity` implies, so the auth entity starts
    # before the edges.
    config = appmodel.with_auth_connect_points(config)
    # And the monitor's, so it comes up before the entities that report to it.
    config = appmodel.with_monitoring_connect_points(config)
    services = {e.get("name") for e in appmodel.entities(config)
                if appmodel.is_service(e)}
    after: Dict[str, set] = {name: set() for name in services}
    for connect_point in config.get("connect_points", []):
        owner = connect_point.get("owner")
        for consumer in connect_point.get("consumers", []):
            if consumer in services and owner in services and owner != consumer:
                after[consumer].add(owner)  # consumer must start after its owner

    ordered: List[str] = []
    placed: set = set()
    remaining = set(services)
    while remaining:
        ready = sorted(name for name in remaining if after[name] <= placed)
        if not ready: # a cycle: place the rest deterministically rather than hang
            ready = sorted(remaining)
        for name in ready:
            ordered.append(name)
            placed.add(name)
            remaining.discard(name)
    return ordered


def serve(project_dir: os.PathLike[str] | str, *, profile: Optional[str] = None) -> str:
    """Launch the built entities in dependency order, and report what is missing to build."""
    from . import build as buildmod
    from . import mesh
    root = Path(project_dir)
    config = buildmod.load_config(root, profile)
    order = startup_order(config)

    lines = ["Startup order (owners before consumers):", "  " + " -> ".join(order) or "  (none)"]
    missing: List[str] = []
    launched: List[str] = []
    env = launch_env(root)
    for name in order:
        # Resolve the suffix rather than naming build/<entity>/<entity> directly.
        binary = _deployed_binary(root, name)
        if binary is None:
            missing.append(name)
            continue
        # From the project root, like `synqt dev`: every relative default in a generated
        # main (the bundle, build/<entity>/topology.json, the certificate, the env file) is
        # project-root relative.
        subprocess.Popen([str(binary)], cwd=str(root), env=env)
        launched.append(name)

    if missing:
        lines.append("")
        lines.append("Not yet built (run 'synqt build' first): " + ", ".join(missing))
    if launched:
        lines.append("Launched: " + ", ".join(launched)
                     + ". The web edge binds the public port; others bind loopback.")
    lines.append("The edge serves each scope the bundle `bundles:` maps it to, or the one "
                 "client's (a CDN serves it under split origin).")
    return "\n".join(lines)


def _bundle_arguments(root: Path, edge: Dict[str, Any],
                      config: Dict[str, Any]) -> List[str]:
    """The `--bundle` arguments one edge is launched with.

    Without `bundles:`, the bare client directory (the single-bundle shorthand). With it,
    one `<scope>=<dir>` per scope: a static bundle inside the edge folder, or a built
    client.
    """
    if not isinstance(edge.get("bundles"), dict) or not edge["bundles"]:
        client = appmodel.client_entity(config)
        folder = appmodel.bundle_output_dir(config, client) if client else "build/client"
        return ["--bundle", str(root / folder)]
    clients = {str(entity.get("name") or ""): entity
               for entity in appmodel.entities(config) if appmodel.is_client(entity)}
    arguments: List[str] = []
    for scope, (kind, value) in sorted(appmodel.bundles_for(config, edge).items()):
        if kind == appmodel.BUNDLE_STATIC:
            directory = root / appmodel.entity_dir(edge) / value
        else:
            client = clients.get(value)
            if client is None:
                # Refused by `synqt check`; skipped here.
                continue
            directory = root / appmodel.bundle_output_dir(config, client)
        arguments += ["--bundle", f"{scope}={directory}"]
    return arguments


def dev_command(root: Path, entity: Dict[str, Any], config: Dict[str, Any],
                port: int, profile_name: str = "debug",
                dev_tools: bool = True, identity_picker: bool = False) -> List[str]:
    """The argv to launch one entity for `synqt dev` (plaintext localhost), run from the
    project root. The edge gets the bundle, the Source QML directory and the dev port; a
    service gets its topology JSON.
    """
    name = entity.get("name")
    resolved = host_binary(root, name, profile_name, dev_tools)
    binary = str(resolved) if resolved else str(
        host_build_dir(root, profile_name, dev_tools) / name)
    if appmodel.is_edge(entity):
        command = ([binary] + _bundle_arguments(root, entity, config)
                   + ["--qml-dir", str(root / appmodel.GENERATED_DIR),
                      "--port", str(port), "--dev"])
        # Beside --dev: --dev allows a development sign-in, this picks which one. Only a
        # SYNQT_DEV_TOOLS build has the picker.
        if identity_picker:
            command.append("--identity-picker")
            # The people from `.dev-identities`, resolved here against the project scopes.
            # Problems are passed along for the picker page to show.
            command += devidentities.for_project(root, config)[0]
        return command
    if appmodel.entity_type(entity) == "monitor":
        # Both halves: the mesh point needs its topology and the generated Source QML, the
        # console needs the bundle arguments and its own `public:` port.
        return ([binary, "--topology", str(root / "build" / name / "topology.json"),
                 "--qml-dir", str(root / appmodel.GENERATED_DIR),
                 "--port", str(appmodel.public_settings(entity).get("port") or 8443)]
                + _bundle_arguments(root, entity, config))
    command = [binary, "--topology", str(root / "build" / name / "topology.json")]
    # pragma Shared QML resolves against the mirror under generated/.
    if appmodel.discover_singletons(root / appmodel.entity_dir(entity)):
        command += ["--qml-dir", str(root / appmodel.GENERATED_DIR)]
    # The auth entity holds the identity engine, so it carries the dev-stub gate. `synqt
    # serve` passes no arguments, so the stub never runs there.
    if appmodel.provider_entity(config) == name:
        command.append("--dev")
    return command


def _launch_order(config: Dict[str, Any]) -> List[str]:
    """Service launch order for dev: owners before consumers, the edge last (public port)."""
    edge = _edge_entity(config)
    edge_name = edge.get("name") if edge else None
    order = startup_order(config)
    return [name for name in order if name != edge_name] + ([edge_name] if edge_name else [])


def _launch_entities(root: Path, config: Dict[str, Any], launch_order: List[str],
                     port: int, profile_name: str = "debug",
                     identity_picker: bool = False
                     ) -> Tuple[List[Tuple[str, subprocess.Popen]], List[str]]:
    """Start each entity for `synqt dev` (plaintext localhost). Returns the running processes
    and the entities not built yet.
    """
    processes: List[Tuple[str, subprocess.Popen]] = []
    missing: List[str] = []
    env = launch_env(root)
    if appmodel.has_dev_stub(config):
        # The development sign-in shared secret, minted per run and passed to every process
        # started here, so no other local process can redeem a code. Unset, both ends read
        # the same empty string.
        env[appmodel.DEV_STUB_SECRET_VARIABLE] = secrets.token_urlsafe(24)
    for name in launch_order:
        entity = next(e for e in config["entities"] if e.get("name") == name)
        # The development tree, which only `synqt dev` builds and launches.
        if host_binary(root, name, profile_name, dev_tools=True) is None:
            missing.append(name)
            continue
        processes.append((name, subprocess.Popen(
            dev_command(root, entity, config, port, profile_name, dev_tools=True,
                        identity_picker=identity_picker),
            cwd=str(root), env=env)))
    return processes, missing


def _terminate(processes: List[Tuple[str, subprocess.Popen]]) -> None:
    """Stop the child processes, escalating to kill if a process does not exit promptly."""
    for _, process in processes:
        process.terminate()
    for _, process in processes:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


def dev_summary(config: Dict[str, Any], url: str, launched: List[str]) -> str:
    """What `synqt dev` prints once everything is up, including who the development sign-in
    offers.
    """
    summary = (f"synqt dev: serving {url} (plaintext localhost).\n"
               f"  Launched: {', '.join(launched)} (edge last, on the dev port).")
    if appmodel.has_dev_stub(config):
        people = ", ".join(user.get("login") or user.get("sub", "")
                           for user in appmodel.dev_stub_users(config))
        summary += (f"\n  Development sign-in on port {appmodel.dev_stub_port(config)}: "
                    f"{people}. Not in a build.")
    return summary


def dev(project_dir: os.PathLike[str] | str, *, profile_name: str = "debug",
        port: int = 8080,
        open_browser: bool = True, block: bool = True, client: str = "wasm",
        watch: bool = True, profile: Optional[str] = None,
        identity_picker: bool = False) -> str:
    """Serve the built client at the web edge over plaintext localhost and open a browser.

    Owners start before consumers; the edge last, serving build/client/. With block=True
    this runs until Ctrl-C and stops the children. With block=False it returns after
    launching (for tests). With block and watch, every relevant edit triggers an incremental
    rebuild and a browser reload.
    """
    from . import build as buildmod
    root = Path(project_dir)
    config = buildmod.load_config(root, profile)
    edge = _edge_entity(config)
    if edge is None:
        return "synqt dev: no web_edge entity in the topology; nothing to serve."

    if identity_picker:
        # Also printed here, before a browser is opened.
        problems = devidentities.for_project(root, config)[1]
        for problem in problems:
            print(f"synqt dev: {problem}")
        # The file names real people and must not be committed. Added through the mesh
        # tooling's ignore rule writer.
        if devidentities.path_of(root).exists():
            mesh.ensure_gitignored(root)

    launch_order = _launch_order(config)
    processes, missing = _launch_entities(root, config, launch_order, port, profile_name,
                                          identity_picker)
    if missing:
        _terminate(processes)
        return ("synqt dev: these entities are not built (run 'synqt build' first): "
                + ", ".join(missing))

    # Give the served bundles their dev live-reload hook before the browser opens.
    _write_dev_reload_harnesses(root, config)

    url = f"http://127.0.0.1:{port}/"
    _wait_for_port(port)
    if open_browser:
        webbrowser.open(url)

    summary = dev_summary(config, url, [name for name, _ in processes])
    if not block:
        return summary

    print(summary)
    if watch:
        names = " and ".join(configmod.config_filenames(profile))
        print(f"  Watching *.qml and {names} for changes (hot reload on). "
              "Press Ctrl-C to stop.")
        # `profile` reloads synqt.yaml; `profile_name` sends the rebuild into the tree these
        # processes run from.
        state = {"processes": processes, "config": config, "profile": profile,
                 "profile_name": profile_name,
                 # Relaunch with the same flags `dev` started with.
                 "identity_picker": identity_picker}
        _watch_loop(root, state, port, client)
        return "synqt dev: stopped."

    print("  Press Ctrl-C to stop.")
    try:
        processes[-1][1].wait()  # the edge, and block until it exits
    except KeyboardInterrupt:
        pass
    finally:
        _terminate(processes)
    return "synqt dev: stopped."


# watch and hot reload

class SourceWatcher:
    """Poll the project sources for changes.

    Watches ``*.qml`` and ``*.syn`` and the configuration files in play, ignoring
    ``generated/``, ``build/``, ``synqt/`` and ``.git``. ``config_names`` includes the
    active profile file, since it is part of the topology.
    """

    _IGNORED_DIRS = {appmodel.GENERATED_DIR, "build", ".git", "synqt", "node_modules",
                     "toolchain"}
    _WATCHED_SUFFIXES = {".qml"}

    def __init__(self, root: os.PathLike[str] | str,
                 config_names: Tuple[str, ...] = ("synqt.yaml",)) -> None:
        self._root = Path(root)
        self._config_names = set(config_names)
        self._snapshot: Dict[Path, int] = self._scan()

    def _scan(self) -> Dict[Path, int]:
        found: Dict[Path, int] = {}
        for dirpath, dirnames, filenames in os.walk(self._root):
            dirnames[:] = [d for d in dirnames
                           if d not in self._IGNORED_DIRS and not d.startswith(".")]
            for filename in filenames:
                path = Path(dirpath) / filename
                if path.suffix in self._WATCHED_SUFFIXES or filename in self._config_names:
                    try:
                        found[path] = path.stat().st_mtime_ns
                    except OSError:
                        pass
        return found

    def poll(self) -> Set[Path]:
        """Rescan and return the watched files created, modified or deleted since the last poll."""
        current = self._scan()
        changed: Set[Path] = {path for path, mtime in current.items()
                              if self._snapshot.get(path) != mtime}
        changed |= {path for path in self._snapshot if path not in current}
        self._snapshot = current
        return changed


def _categorize(changed: Set[Path], root: Path, config: Dict[str, Any],
                config_names: Tuple[str, ...] = ("synqt.yaml",)) -> Tuple[bool, bool]:
    """Decide whether a change touches the host side, the client side, or both. A configuration
    file touches both. Entity QML belongs to that entity side by folder (`client/app/`,
    `web/edge/`). A file in no entity folder rebuilds both.
    """
    folders = [(Path(appmodel.entity_dir(entity)).parts, appmodel.is_client(entity))
               for entity in appmodel.entities(config)]
    host = client = False
    for path in changed:
        if path.name in config_names:
            return True, True
        try:
            parts = path.relative_to(root).parts
        except ValueError:
            host = client = True
            continue
        owners = [is_client for folder, is_client in folders
                  if parts[:len(folder)] == folder]
        if not owners:
            host = client = True
        elif all(owners):
            client = True
        elif not any(owners):
            host = True
        else:
            host = client = True
    return host, client


def _watch_loop(root: Path, state: Dict[str, Any], port: int, client: str) -> None:
    """Watch sources until the edge exits or Ctrl-C: rebuild and reload on every change."""
    watcher = SourceWatcher(root, configmod.config_filenames(state.get("profile")))
    try:
        while True:
            if state["processes"][-1][1].poll() is not None:
                break  # the edge exited on its own
            changed = watcher.poll()
            if changed:
                _hot_reload(root, state, port, client, changed)
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        _terminate(state["processes"])


def _hot_reload(root: Path, state: Dict[str, Any], port: int, client: str,
                changed: Set[Path]) -> None:
    """Rebuild for a detected change, restart the host entities if a host target changed, and
    bump the reload token.
    """
    from . import build as buildmod
    names = ", ".join(sorted(path.name for path in changed)[:4])
    print(f"synqt dev: change detected ({names}); rebuilding...")

    config_names = configmod.config_filenames(state.get("profile"))
    touched_config = sorted(path.name for path in changed if path.name in config_names)
    if touched_config:
        # Re-read the topology. A config that does not parse is reported and the running
        # system is left alone.
        try:
            state["config"] = buildmod.load_config(root, state.get("profile"))
        except (OSError, yaml.YAMLError, configmod.ConfigError) as error:
            print(f"  {touched_config[0]}: {error}\n"
                  "  (keeping the running processes; fix and save again)")
            return
    config = state["config"]

    # A failed rebuild is reported and the running system stays up; `synqt build` raises
    # instead. BuildError and AppGenError get their own message (compile_incremental
    # regenerates first, so a half-written route raises AppGenError). The broad fallback
    # keeps the session alive on anything else, such as an AttributeError from half-typed
    # YAML. It does not catch KeyboardInterrupt or SystemExit.
    try:
        note, _, _ = buildmod.compile_incremental(root, config, client=client,
                                                  profile_name=state.get("profile_name",
                                                                         "debug"))
    except (buildmod.BuildError, appmodel.AppGenError) as error:
        print(f"  {error}\n  (keeping the running processes; fix and save again)")
        return
    except Exception as error:
        print(f"  rebuild failed: {error}\n"
              "  (keeping the running processes; fix and save again)")
        return

    host_changed, _ = _categorize(changed, root, config, config_names)
    if host_changed:
        _terminate(state["processes"])
        processes, missing = _launch_entities(
            root, config, _launch_order(config), port,
            state.get("profile_name", "debug"), state.get("identity_picker", False))
        state["processes"] = processes
        if missing:
            print("  not built after rebuild: " + ", ".join(missing))
            return
        _wait_for_port(port)

    # A wasm rebuild rewrote the prod index.html. Re-inject the hook and bump the token.
    _write_dev_reload_harnesses(root, config)
    print("  rebuilt; the browser will reload.")


def _write_dev_reload_harnesses(root: Path, config: Dict[str, Any]) -> None:
    """Install the live-reload hook into every browser client's bundle."""
    from . import build as buildmod
    for folder in buildmod.client_bundle_targets(config).values():
        _write_dev_reload_harness(root / folder)


def _write_dev_reload_harness(client_dir: os.PathLike[str] | str) -> None:
    """Install the dev live-reload hook into the served bundle: synqt-dev.js, a reference in
    index.html (idempotent), and a fresh reload token the browser polls.
    """
    client_dir = Path(client_dir)
    if not client_dir.exists():
        return
    (client_dir / "synqt-dev.js").write_text(clientshell.render_dev_reload_js(), encoding="utf-8")
    index = client_dir / "index.html"
    if index.exists():
        html = index.read_text(encoding="utf-8")
        if 'src="synqt-dev.js"' not in html:
            html = html.replace("</body>",
                                '  <script src="synqt-dev.js"></script>\n</body>')
            index.write_text(html, encoding="utf-8")
    (client_dir / "synqt-reload.txt").write_text(f"{time.time_ns()}\n", encoding="utf-8")


def _wait_for_port(port: int, *, timeout_s: float = 10.0) -> bool:
    """Wait until something accepts on the local dev port (the edge is listening)."""
    import socket
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.5)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.2)
    return False


def test(project_dir: os.PathLike[str] | str) -> int:
    """Build and run the project's CTest suite. Returns the process exit code."""
    root = Path(project_dir)
    if not shutil.which("ctest"):
        print("synqt test: ctest not found (install CMake).")
        return 1

    # No tests is not a failure. Say so instead of running ctest over zero tests.
    if not appmodel.test_qml_files(root):
        print("synqt test: this project has no tests yet.\n"
              "  A test is a QML file at tests/tst_<Something>.qml driving one connect\n"
              "  point's Source through the SynQt.Test harness. See "
              "https://synqt.org/testing/.")
        return 0

    # The default profile tree. App tests need nothing from SYNQT_DEV_TOOLS.
    host_build = host_build_dir(root, "debug")
    if not (host_build / "CTestTestfile.cmake").exists():
        print("synqt test: no configured test build. Run 'synqt build' first "
              "(the host preset configures the test targets).")
        return 1

    # Build the test target here: `synqt build` builds entity targets by name, and ctest
    # builds nothing.
    cmake = shutil.which("cmake")
    if cmake is None:
        print("synqt test: cmake not found (install CMake).")
        return 1
    compiled = subprocess.run([cmake, "--build", str(host_build),
                               "--target", cmakegen.TESTS_TARGET])
    if compiled.returncode != 0:
        print("synqt test: the tests did not build; nothing was run.")
        return compiled.returncode

    result = subprocess.run(["ctest", "--test-dir", str(host_build), "--output-on-failure"])
    return result.returncode
