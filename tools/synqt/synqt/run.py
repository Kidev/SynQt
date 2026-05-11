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
from . import clientshell, cmakegen, config as configmod, devidentities, profiles, toolchain


def launch_env(root: Path) -> Dict[str, str]:
    """The environment an entity binary is launched with.

    On Windows this has to carry the Qt kit's bin directory on PATH. Windows has no RPATH: a
    process resolves a DLL through its own directory and then PATH, so an entity built into
    build/host/ cannot see Qt6Core.dll in the kit's bin and dies before main(), typically
    behind an error dialog rather than a message. Linux and macOS bake the kit's path into the
    binary at link time and need nothing here, which is why this only ever shows up on the one
    platform, and only when running, never when building.
    """
    env = dict(os.environ)
    # Asked, not assumed, and asked of the one function that answers it. A second way to
    # detect the host is how the first one goes stale (toolchain.host_platform() is already
    # the single source of truth, and build.desktop_platform() defers to it too).
    if toolchain.host_platform() != "windows":
        return env
    host_qt = resolved_host_qt(root)
    if host_qt:
        env["PATH"] = str(Path(host_qt) / "bin") + os.pathsep + env.get("PATH", "")
    return env


def resolved_host_qt(root: Path) -> Optional[str]:
    """The resolved host Qt kit for this project, or None. Split out so launch_env can be
    tested without a toolchain on disk."""
    return toolchain.resolve(root).get("host_qt")


def _executable(directory: Path, name: str) -> Optional[Path]:
    """The executable called `name` in `directory`, or None. The suffix is resolved rather
    than assumed, because only Windows adds one (.exe). Looking for the bare name there finds
    nothing and reports every entity of a perfectly good build as missing, which reads like a
    broken compile rather than a naming convention.

    The macOS desktop client is an .app bundle (cmakegen sets MACOSX_BUNDLE so the
    macdeployqt hand-off in docs/desktop.md is possible at all), and the thing to *run* is the
    executable inside it, not the directory. Resolving that here keeps every caller (`synqt
    serve`, `synqt dev --desktop`, the deploy install) working off one answer.
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
    """Where one profile's host binaries are.

    Each profile builds into its own directory (see profiles.build_dir), so "where is the
    binary" is no longer one answer and every caller has to say which build it means.
    """
    return root / profiles.build_dir("host", profile_name, dev_tools=dev_tools)


def host_binary(root: Path, name: str, profile_name: str = "debug",
                dev_tools: bool = False) -> Optional[Path]:
    """The compiled host executable for one entity, or None if that profile never built it."""
    return _executable(host_build_dir(root, profile_name, dev_tools), name)


def built_profiles(root: Path, name: str) -> List[str]:
    """Which profiles have this entity built, so a miss can say what is there instead.

    Without it, asking for a profile nobody built reports the entity as never built, which
    sends the reader looking for a build failure that did not happen.
    """
    found = []
    for candidate in profiles.PROFILES:
        for dev in (False, True):
            if host_binary(root, name, candidate, dev):
                found.append(candidate + ("-dev" if dev else ""))
    return found


def host_artifact(root: Path, name: str, profile_name: str = "debug",
                  dev_tools: bool = False) -> Optional[Path]:
    """What a deploy step should copy for one entity. The .app bundle on macOS, otherwise the
    executable itself.

    Distinct from host_binary() because the two answers differ on exactly one platform, and
    conflating them there loses the bundle: copying only `client.app/Contents/MacOS/client`
    produces a file that cannot be launched as an app, cannot be signed, and is not what
    macdeployqt operates on.
    """
    bundle = host_build_dir(root, profile_name, dev_tools) / f"{name}.app"
    if bundle.is_dir():
        return bundle
    return host_binary(root, name, profile_name, dev_tools)


def _deployed_binary(root: Path, name: str) -> Optional[Path]:
    """The entity's executable in its own deploy directory (what `synqt build` installs and
    `synqt serve` launches), or None if it was never built."""
    return _executable(root / "build" / name, name)


def _edge_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return next((e for e in appmodel.entities(config) if appmodel.is_edge(e)), None)


def startup_order(config: Dict[str, Any]) -> List[str]:
    """Service entities ordered so every owner starts before its consumers."""
    # Including the links `identity.provider_entity` implies, so an auth entity comes up
    # before the edges that reach it for a login.
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
        # Ask for the deployed binary by name and let the suffix be resolved, rather than
        # naming build/<entity>/<entity> directly. Only Windows adds .exe, and looking for the
        # bare name there reports every entity of a perfectly good build as unbuilt.
        binary = _deployed_binary(root, name)
        if binary is None:
            missing.append(name)
            continue
        # From the project root, like `synqt dev`, because every relative default a
        # generated main carries is project-root relative. The bundle (build/client), the
        # entity's topology (build/<entity>/topology.json), the public certificate and the
        # env file, all spelled the way synqt.yaml spells them. Run from the deploy
        # directory instead and the edge looks for the bundle under build/<entity>/.
        subprocess.Popen([str(binary)], cwd=str(root), env=env)
        launched.append(name)

    if missing:
        lines.append("")
        lines.append("Not yet built (run 'synqt build' first): " + ", ".join(missing))
    if launched:
        lines.append("Launched: " + ", ".join(launched)
                     + ". The web edge binds the public port; others bind loopback.")
    lines.append("The edge serves the client from build/client/ (or a CDN for split-origin).")
    return "\n".join(lines)


def _bundle_arguments(root: Path, edge: Dict[str, Any],
                      config: Dict[str, Any]) -> List[str]:
    """The `--bundle` arguments one edge is launched with.

    An edge with no `bundles:` block is passed the bare directory it has always been
    passed, which the generated main reads as the single-bundle shorthand. An edge that
    declares the block is passed one `<scope>=<dir>` per mapped scope: a static bundle
    resolves inside the edge's own folder, a client bundle to where the build assembled
    that client.
    """
    if not isinstance(edge.get("bundles"), dict) or not edge["bundles"]:
        return ["--bundle", str(root / "build" / "client")]
    clients = {str(entity.get("name") or ""): entity
               for entity in appmodel.entities(config) if appmodel.is_client(entity)}
    arguments: List[str] = []
    for scope, (kind, value) in sorted(appmodel.bundles_for(config, edge).items()):
        if kind == appmodel.BUNDLE_STATIC:
            directory = root / appmodel.entity_dir(edge) / value
        else:
            client = clients.get(value)
            if client is None:
                # Refused by `synqt check`. Skipped here rather than launching an edge
                # pointed at a directory no build ever writes.
                continue
            directory = root / appmodel.bundle_output_dir(config, client)
        arguments += ["--bundle", f"{scope}={directory}"]
    return arguments


def dev_command(root: Path, entity: Dict[str, Any], config: Dict[str, Any],
                port: int, profile_name: str = "debug",
                dev_tools: bool = True, identity_picker: bool = False) -> List[str]:
    """The argv to launch one entity for `synqt dev` (plaintext localhost), run from the
    project root so the relative bundle/topology defaults resolve. The edge gets the
    served bundle, the owner Source QML directory, and the dev port. A service gets its
    resolved topology JSON."""
    name = entity.get("name")
    resolved = host_binary(root, name, profile_name, dev_tools)
    binary = str(resolved) if resolved else str(
        host_build_dir(root, profile_name, dev_tools) / name)
    if appmodel.is_edge(entity):
        command = ([binary] + _bundle_arguments(root, entity, config)
                   + ["--qml-dir", str(root / appmodel.GENERATED_DIR),
                      "--port", str(port), "--dev"])
        # Beside --dev rather than instead of it: --dev is what makes any synthesized
        # identity possible at all, and this only chooses which development sign-in is
        # served. The binary this launches was built with SYNQT_DEV_TOOLS, so it is the
        # only kind of edge that has a picker to be asked for.
        if identity_picker:
            command.append("--identity-picker")
            # The named people from `.dev-identities`, resolved here because this side owns
            # the YAML parser and knows which scopes the project declares. Whatever did not
            # survive that reading rides along as a problem the picker prints on its own
            # page, where the developer wondering why Alice is missing is looking.
            command += devidentities.for_project(root, config)[0]
        return command
    if appmodel.entity_type(entity) == "monitor":
        # Both halves. The mesh point it hosts needs its topology and the Source QML the
        # generator mirrored under generated/, and the console it serves needs the same
        # bundle arguments an edge gets. Its own port, from `public:`, because it is a
        # second browser-facing server and must not be handed the edge's.
        return ([binary, "--topology", str(root / "build" / name / "topology.json"),
                 "--qml-dir", str(root / appmodel.GENERATED_DIR),
                 "--port", str(appmodel.public_settings(entity).get("port") or 8443)]
                + _bundle_arguments(root, entity, config))
    command = [binary, "--topology", str(root / "build" / name / "topology.json")]
    # A service that declares pragma-Singleton QML resolves it against the mirror under
    # generated/, which is where the loadable copy of every entity's QML lives.
    if appmodel.discover_singletons(root / appmodel.entity_dir(entity)):
        command += ["--qml-dir", str(root / appmodel.GENERATED_DIR)]
    # The auth entity holds the identity engine, so it carries the dev-stub gate the edge
    # carries in-process. `synqt serve` passes no arguments at all, which is what keeps the
    # stub out of anything that ships.
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
    """Start each entity for `synqt dev` (plaintext localhost). Returns the running
    processes and the names of any entity whose binary is not built yet."""
    processes: List[Tuple[str, subprocess.Popen]] = []
    missing: List[str] = []
    env = launch_env(root)
    if appmodel.has_dev_stub(config):
        # The shared secret the development sign-in's token endpoint checks, minted per
        # run and given to everything this starts. Not a credential in any real sense (a
        # fake provider on loopback), but a fresh one per run means another process on
        # this machine cannot spend a code against it, and both ends read one variable so
        # there is nothing to keep in step. Unset, both would read the same empty string
        # and still agree, which is what makes running an edge with --dev by hand work.
        env[appmodel.DEV_STUB_SECRET_VARIABLE] = secrets.token_urlsafe(24)
    for name in launch_order:
        entity = next(e for e in config["entities"] if e.get("name") == name)
        # The development tree, always. `synqt dev` is the only command that builds one
        # and the only one that launches from it.
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
    """What `synqt dev` says once everything is up.

    The development sign-in is named here because it is the one thing about a dev run that
    is invisible from the page. Who it offers, and that none of it is in a build.
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

    Owners start before consumers. The edge comes up last and serves build/client/. With
    block=True this runs until interrupted (Ctrl-C), tearing down the child processes. With
    block=False it returns immediately after launching (used by tests). When block and watch
    are both set, the project sources are watched and every relevant edit triggers an
    incremental rebuild plus an automatic browser reload."""
    from . import build as buildmod
    root = Path(project_dir)
    config = buildmod.load_config(root, profile)
    edge = _edge_entity(config)
    if edge is None:
        return "synqt dev: no web_edge entity in the topology; nothing to serve."

    if identity_picker:
        # Said here as well as on the picker's page, because the two readers are different
        # people at different moments. This one is whoever started `synqt dev` and can
        # fix the file before opening a browser at all.
        problems = devidentities.for_project(root, config)[1]
        for problem in problems:
            print(f"synqt dev: {problem}")
        # And the file itself must never be committed. It names the people one developer
        # signs in as. Registered through the mesh tooling's rule writer rather than a
        # second one, so a project has one list of what git must not take.
        if devidentities.path_of(root).exists():
            mesh.ensure_gitignored(root)

    launch_order = _launch_order(config)
    processes, missing = _launch_entities(root, config, launch_order, port, profile_name,
                                          identity_picker)
    if missing:
        _terminate(processes)
        return ("synqt dev: these entities are not built (run 'synqt build' first): "
                + ", ".join(missing))

    # Give the served bundle its dev live-reload hook before the browser opens.
    _write_dev_reload_harness(root / "build" / "client")

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
        # `profile` is the configuration layer and `profile_name` is the build profile.
        # The watcher needs both. The first to reload synqt.yaml, the second so a rebuild
        # goes into the tree these processes were launched from.
        state = {"processes": processes, "config": config, "profile": profile,
                 "profile_name": profile_name,
                 # Carried here so a hot reload relaunches with the same flags it started
                 # with. A relaunch that builds its own argv from the defaults
                 # silently drops whatever `dev` was asked
                 # for and puts a differently-configured edge back on the same port.
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
    """Poll the project's edit surface for changes. Watches ``*.qml`` and ``*.syn`` sources
    and the configuration files in play. Ignores generated output and tooling
    (``generated/``, ``build/``, ``synqt/``, ``.git``). Everything SynQt writes is under
    ``generated/``, so a rebuild that rewrites it cannot re-trigger the watcher.

    ``config_names`` carries the active profile's file as well as ``synqt.yaml``: under
    ``--profile production`` the profile file is part of the topology, and a watcher that
    did not know it would keep serving the old wiring with nothing to say it had missed
    the save."""

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
        """Rescan and return the set of created, modified, or deleted watched files since
        the previous poll, updating the snapshot."""
        current = self._scan()
        changed: Set[Path] = {path for path, mtime in current.items()
                              if self._snapshot.get(path) != mtime}
        changed |= {path for path in self._snapshot if path not in current}
        self._snapshot = current
        return changed


def _categorize(changed: Set[Path], root: Path, config: Dict[str, Any],
                config_names: Tuple[str, ...] = ("synqt.yaml",)) -> Tuple[bool, bool]:
    """Decide whether a change touches the host side (services/edge), the client side, or
    both. A configuration file is the topology and what crosses every link, so a change to
    one affects both. An entity's QML is attributed to that entity's side."""
    services = {e.get("name") for e in appmodel.entities(config)
                if appmodel.is_service(e)}
    client_name = next((e.get("name") for e in appmodel.entities(config)
                        if appmodel.is_client(e)), None)
    host = client = False
    for path in changed:
        if path.name in config_names:
            return True, True
        try:
            top = path.relative_to(root).parts[0]
        except ValueError:
            host = client = True
            continue
        if top == client_name:
            client = True
        elif top in services:
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
    """Rebuild for a detected change, restart the host entities if a host target changed,
    and bump the reload token so the browser refreshes."""
    from . import build as buildmod
    names = ", ".join(sorted(path.name for path in changed)[:4])
    print(f"synqt dev: change detected ({names}); rebuilding...")

    config_names = configmod.config_filenames(state.get("profile"))
    touched_config = sorted(path.name for path in changed if path.name in config_names)
    if touched_config:
        # Re-read. The topology may have gained or lost entities. A half-typed YAML is the
        # same kind of news as a failed rebuild below, so it is reported and the running
        # system is left alone rather than rebuilt against a topology that no longer parses.
        try:
            state["config"] = buildmod.load_config(root, state.get("profile"))
        except (OSError, yaml.YAMLError, configmod.ConfigError) as error:
            print(f"  {touched_config[0]}: {error}\n"
                  "  (keeping the running processes; fix and save again)")
            return
    config = state["config"]

    # A failed rebuild is fatal to `synqt build` and merely news to `synqt dev`: the whole
    # point of the watcher is that you fix the typo and save again, so the running system
    # stays up and the error is reported. (`build` raises for the opposite reason: it must
    # never report success for a binary it did not produce.)
    #
    # BuildError and AppGenError get their own clear message: compile_incremental regenerates
    # before it compiles, so a config the generator refuses (a route saved before its `view`
    # is typed) raises AppGenError, and that edit is exactly the one the watcher exists for.
    #
    # The broad fallback below is intended. In dev the contract is always "report and keep
    # running", never "crash". A half-typed synqt.yaml can parse as valid YAML yet put a
    # scalar where the generator expects a mapping (`router: /home` before its indented
    # `fallback:` is typed), which reaches the generator and raises a bare AttributeError
    # that no narrow tuple lists. Letting any such exception out would hit `_watch_loop`'s
    # finally and tear down every child process on a half-finished save. `except Exception`
    # reports the message and survives. It does not catch KeyboardInterrupt or SystemExit
    # (those are BaseException, not Exception), so Ctrl-C still stops the session cleanly.
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
    _write_dev_reload_harness(root / "build" / "client")
    print("  rebuilt; the browser will reload.")


def _write_dev_reload_harness(client_dir: os.PathLike[str] | str) -> None:
    """Install the dev live-reload hook into the served bundle: the external synqt-dev.js,
    a one-line reference in index.html (idempotent), and a fresh reload token the browser
    polls. Bumping the token is what triggers the automatic reload."""
    client_dir = Path(client_dir)
    if not client_dir.exists():
        return
    (client_dir / "synqt-dev.js").write_text(clientshell.render_dev_reload_js())
    index = client_dir / "index.html"
    if index.exists():
        html = index.read_text()
        if 'src="synqt-dev.js"' not in html:
            html = html.replace("</body>",
                                '  <script src="synqt-dev.js"></script>\n</body>')
            index.write_text(html)
    (client_dir / "synqt-reload.txt").write_text(f"{time.time_ns()}\n")


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

    # A project with no tests is the ordinary state of a new one, and it is not a failure.
    # Reaching ctest would report a passing run over zero tests, which reads exactly
    # like a suite that ran, so say what is true and where to start.
    if not appmodel.test_qml_files(root):
        print("synqt test: this project has no tests yet.\n"
              "  A test is a QML file at tests/tst_<Something>.qml driving one connect\n"
              "  point's Source through the SynQt.Test harness. See "
              "https://synqt.org/testing/.")
        return 0

    # The default profile's tree, which is what `synqt build` with no flag configures. Not
    # the development one: an app test drives a connect point's Source through SynQt.Test
    # and needs nothing that SYNQT_DEV_TOOLS adds.
    host_build = host_build_dir(root, "debug")
    if not (host_build / "CTestTestfile.cmake").exists():
        print("synqt test: no configured test build. Run 'synqt build' first "
              "(the host preset configures the test targets).")
        return 1

    # Compiled here rather than by `synqt build`, which builds the entity targets by name
    # and never named this one: ctest builds nothing, so `synqt build && synqt test` ended
    # in "Unable to find executable" over a target nothing had ever been asked to make.
    # Building it on the way to running it also keeps `synqt dev`'s rebuild loop to the
    # entities, which is what a save is usually about.
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
