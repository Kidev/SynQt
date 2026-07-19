# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt check``: validate config and topology (fail fast before a build)."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml

from . import addentity, clientcache, toolchain


def validate(config: Dict[str, Any]) -> Tuple[bool, List[str]]:
    """Return (ok, messages). Messages prefixed 'error:' fail the build; 'warn:' do not."""
    messages: List[str] = []
    entities = {e.get("name"): e for e in config.get("entities", []) if isinstance(e, dict)}
    if not entities:
        return False, ["error: no entities declared"]

    web_edges = {name for name, e in entities.items()
                 if e.get("capability") == "web_edge" or e.get("web_edge")}
    clients = {name for name, e in entities.items() if e.get("kind") == "client"}

    for connect_point in config.get("connect_points", []):
        name = connect_point.get("name", "<unnamed>")
        owner = connect_point.get("owner")
        consumers = connect_point.get("consumers", [])

        if owner not in entities:
            messages.append(f"error: connect point '{name}' has unknown owner '{owner}'")
        for consumer in consumers:
            if consumer not in entities:
                messages.append(
                    f"error: connect point '{name}' has unknown consumer '{consumer}'")
            # The browser can only physically reach a web edge: a client may consume a
            # connect point only if its owner is a web_edge entity.
            if consumer in clients and owner not in web_edges:
                messages.append(
                    f"error: client '{consumer}' consumes '{name}', owned by '{owner}', "
                    "which is not a web_edge entity (the browser can only reach a web edge)")

        # A local-socket link must be opted into explicitly (never picked implicitly), and
        # every local link is surfaced: its Caller.entity is colocation-trusted, not
        # certificate-authenticated, so any same-user process can present that entity name
        # (pitfall 7). Owners that gate a privileged action on entity identity must require
        # Caller.isEntityVerified on such a link.
        transport = connect_point.get("transport")
        if transport == "local":
            if not connect_point.get("transport_local_explicit", True):
                messages.append(f"error: connect point '{name}' uses transport local implicitly")
            else:
                messages.append(
                    f"warn: connect point '{name}' uses transport local: its caller entity is "
                    "colocation-trusted, not certificate-authenticated (gate privileged "
                    "actions on Caller.isEntityVerified)")

    # No provider secret may be reachable from a client target.
    for name in clients:
        entity = entities[name]
        if entity.get("provider") or entity.get("settings"):
            messages.append(f"error: client '{name}' must not carry a provider/secret block")

    # The client build mode and the cross-origin-isolation it requires must pair up
    # (pitfall 13): a multi-threaded WASM client needs SharedArrayBuffer, which the browser
    # grants only to a cross-origin-isolated page.
    threads = str((config.get("build") or {}).get("client_threads", "single")).lower()
    if threads not in ("single", "multi"):
        messages.append(
            f"error: build.client_threads must be 'single' or 'multi', not '{threads}'")
    security = config.get("security") or {}
    if threads == "multi" and security.get("cross_origin_isolation") is False:
        messages.append(
            "warn: build.client_threads is 'multi', which forces cross-origin isolation on; "
            "security.cross_origin_isolation: false is overridden to true")

    # Where the client sends diagnostic output (build.client_logging). Unset is fine: the
    # client defaults to console in a debug build and drops debug output in a release build.
    logging_mode = (config.get("build") or {}).get("client_logging")
    if logging_mode is not None and str(logging_mode).lower() not in ("console", "qt", "none"):
        messages.append(
            f"error: build.client_logging must be 'console', 'qt', or 'none', not "
            f"'{logging_mode}'")

    messages += _loading_messages(config)
    for name in sorted(entities):
        messages += _provider_messages(name, entities[name])

    # How a repeat visitor gets the client back (build.client_cache). Unset is fine: it
    # defaults to the service worker.
    cache_mode = (config.get("build") or {}).get("client_cache")
    if cache_mode is not None and str(cache_mode).lower() not in clientcache.MODES:
        messages.append(
            f"error: build.client_cache must be 'service_worker' or 'http', not "
            f"'{cache_mode}'")

    ok = not any(message.startswith("error:") for message in messages)
    if ok and not messages:
        messages.append("ok: topology valid")
    return ok, messages


def _provider_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A `provider.name` must select something for the entity's blueprint family.

    Left to the runtime this is a poor failure: the name misses, the entity refuses to
    start, and you find out on the next deploy rather than on the next `synqt check`. The
    bundled names come from addentity.PROVIDERS, so the list offered by `synqt add entity`,
    the list accepted by the C++ factories, and the list checked here cannot drift apart.
    """
    provider = entity.get("provider")
    if not isinstance(provider, dict):
        return []
    selected = provider.get("name")
    if selected is None or not str(selected).strip():
        return []  # no name: the family default (sqlite, memory) applies
    selected = str(selected)

    blueprint = entity.get("blueprint")
    family = addentity.BLUEPRINTS.get(blueprint) if blueprint else None
    if family is None:
        # gateway and jobs carry a provider block for their own settings but select no
        # engine; a bare service entity has no family at all.
        return [f"error: entity '{name}' sets provider.name '{selected}' but its blueprint "
                f"('{blueprint or 'none'}') takes no data provider"]

    if selected.startswith(addentity.CUSTOM_PREFIX):
        custom = selected[len(addentity.CUSTOM_PREFIX):]
        if not custom:
            return [f"error: entity '{name}' has a malformed provider.name 'custom:': "
                    "custom: must be followed by the name the provider is registered under"]
        # A registered name is only knowable at run time, so the shape is all that can be
        # checked here; the factory names the registered providers if the lookup misses.
        return []

    if selected not in addentity.PROVIDERS[family]:
        return [f"error: entity '{name}' selects provider.name '{selected}', which is not a "
                f"{family} provider; the bundled {family} providers are "
                f"{', '.join(addentity.PROVIDERS[family])}, or write your own and select it "
                f"with custom:<Name> (see docs/providers.md)"]
    return []


_LOADING_KEYS = ("logo", "icon", "background", "title", "html")
# The contract an html override keeps with the generated boot script.
_LOADING_HOOKS = ("synqt-loading", "synqt-bar", "synqt-status", "screen")


def _loading_messages(config: Dict[str, Any]) -> List[str]:
    """Shape of build.loading. Every message carries the error:/warn: prefix validate()
    derives its result from; an unprefixed one would never fail anything."""
    loading = (config.get("build") or {}).get("loading")
    if loading is None:
        return []
    if not isinstance(loading, dict):
        return ["error: build.loading must be a map"]
    messages: List[str] = []
    for key, value in sorted(loading.items()):
        if key not in _LOADING_KEYS:
            messages.append(f"error: build.loading: unknown key '{key}' "
                            f"(expected one of {', '.join(_LOADING_KEYS)})")
        elif not isinstance(value, str) or not value.strip():
            messages.append(f"error: build.loading.{key} must be a non-empty string")
    if "html" in loading:
        ignored = sorted(set(loading) & {"logo", "icon", "background", "title"})
        if ignored:
            messages.append(
                f"error: build.loading.html replaces the whole page, so "
                f"{', '.join(ignored)} would be ignored; remove either html or those keys")
    return messages


def lint_loading(project_dir) -> List[str]:
    """Check that build.loading's files exist and that an html override keeps its
    contract with the boot script.

    Separate from validate() because it needs the project directory: a logo naming a
    file that is not there, or an override missing the ids the boot script drives, both
    produce a bundle that builds and then fails in the browser.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    loading = ((config or {}).get("build") or {}).get("loading")
    if not isinstance(loading, dict):
        return []

    messages: List[str] = []
    for key in ("logo", "icon", "html"):
        value = loading.get(key)
        if isinstance(value, str) and value.strip() and not (root / value).is_file():
            messages.append(f"error: build.loading.{key}: no such file '{value}'")

    override = loading.get("html")
    if isinstance(override, str) and (root / override).is_file():
        page = (root / override).read_text(encoding="utf-8", errors="replace")
        missing = [hook for hook in _LOADING_HOOKS if f'id="{hook}"' not in page]
        if missing:
            messages.append(
                f"error: build.loading.html '{override}' is missing the element id(s) "
                f"{', '.join(missing)} the boot script drives; the app would never show")
        if "synqt-boot.js" not in page:
            messages.append(
                f"error: build.loading.html '{override}' does not load synqt-boot.js; "
                f"the client would never start")
    return messages


# QQmlApplicationEngine shows a root object only if it is a window; anything else loads
# without error and renders nothing. Qt Quick's window types, plus the Controls one.
_WINDOW_ROOTS = ("ApplicationWindow", "Window")

_QML_ROOT = re.compile(r"^\s*([A-Z]\w*)\s*\{", re.MULTILINE)


def _qml_root_type(source: str) -> Optional[str]:
    """The root object's type name, ignoring comments, imports, and pragmas."""
    code = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    code = "\n".join(line.split("//", 1)[0] for line in code.splitlines())
    code = "\n".join(line for line in code.splitlines()
                     if not re.match(r"\s*(import|pragma)\b", line))
    match = _QML_ROOT.search(code)
    return match.group(1) if match else None


def lint_client_root(project_dir) -> List[str]:
    """Check that every client entity's Main.qml root is a window.

    The generated client main.cpp does engine.loadFromModule(uri, "Main"), so Main.qml is
    the root object. A Page or Item root there is the worst kind of defect: it builds, it
    loads, it logs nothing, and the browser shows a blank page. Only a real browser
    catches it otherwise, so it is an error here.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    if not config_path.exists():
        return []
    config = yaml.safe_load(config_path.read_text()) or {}

    messages: List[str] = []
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict) or entity.get("kind") != "client":
            continue
        main = root / str(entity.get("name", "")) / "Main.qml"
        if not main.is_file():
            continue
        found = _qml_root_type(main.read_text(encoding="utf-8", errors="replace"))
        if found is not None and found not in _WINDOW_ROOTS:
            messages.append(
                f"error: {main.relative_to(root)}: the client's root object is "
                f"'{found}', which is not a window ({' or '.join(_WINDOW_ROOTS)}); it "
                "would load without error and render nothing")
    return messages


_CONTRACT_MEMBERS = ("prop", "model", "slot", "signal")


def lint_contracts(project_dir) -> List[str]:
    """Structural lint of shared/*.syn. (The full parse runs in synqtc at build time.)"""
    messages: List[str] = []
    shared = Path(project_dir) / "shared"
    if not shared.exists():
        return messages
    for syn in sorted(shared.glob("*.syn")):
        code = "\n".join(line.split("//", 1)[0] for line in syn.read_text().splitlines())
        if code.count("{") != code.count("}"):
            messages.append(f"error: {syn.name}: unbalanced braces")
        if not re.search(r"\b(contract|record)\b", code):
            messages.append(f"error: {syn.name}: declares no contract or record")
        for block in re.finditer(r"contract\s+\w+\s*\{([^}]*)\}", code, re.S):
            for line in block.group(1).splitlines():
                statement = line.strip()
                if not statement:
                    continue
                if statement.split()[0] not in _CONTRACT_MEMBERS:
                    messages.append(
                        f"error: {syn.name}: unexpected member '{statement[:32]}' "
                        "(want prop/model/slot/signal)")
    return messages


# Categories qmllint reports as warnings that are actually fatal at run time, elevated so
# they fail the check. `property-override`: shadowing a FINAL member (the classic case is a
# delegate taking a model role named x or y as a required property, against the x/y every
# Item already declares FINAL) makes the whole component fail to load with "Cannot override
# FINAL property": a blank page, not a style nit.
_QML_FATAL_CATEGORIES = ("property-override",)


def qt_tool_path(tool: str) -> Optional[str]:
    """A Qt tool (qmllint, qmlformat), from PATH or from the resolved Qt kit.

    They live in the Qt kit's bin, which is usually NOT on PATH, so looking only at PATH
    silently skips the check on most machines.

    The executable suffix is resolved rather than assumed: only Windows adds one, and
    shutil.which() applies PATHEXT for us while a hand-built path does not. Naming the
    bare tool there finds nothing, and this function's contract is that None means "no
    linter installed", so an unresolved suffix would quietly downgrade `synqt check`
    to skipping the QML lint on every Windows machine.
    """
    found = shutil.which(tool)
    if found:
        return found
    kit = toolchain.resolve(Path.cwd()).get("host_qt")
    if kit:
        for suffix in ("", ".exe"):
            candidate = Path(kit) / "bin" / f"{tool}{suffix}"
            if candidate.is_file():
                return str(candidate)
    return None


def qmllint_path() -> Optional[str]:
    return qt_tool_path("qmllint")


def qmlformat_path() -> Optional[str]:
    return qt_tool_path("qmlformat")


def project_qml_files(project_dir) -> List[Path]:
    """The project's own QML: not build output, not vendored dependencies."""
    root = Path(project_dir)
    return [qml for qml in sorted(root.rglob("*.qml"))
            if not ({"build", "node_modules"} & set(qml.parts))]


def wants_qml_format_check(config: Dict[str, Any]) -> bool:
    """Whether the project opted into the qmlformat check (`check.qml_format: true`).

    Off unless asked, which is not timidity. qmlformat reflows expressions and no setting
    stops it, so a project that deliberately wraps a long binding at the meaningful break
    would be told it is wrong on every run, forever. A warning that is always there is a
    warning nobody reads, and this project already shipped a blank page past a check whose
    output people had learned to skim. `synqt new` turns it on, because the QML it
    scaffolds is format-clean from the first commit.
    """
    return bool((config.get("check") or {}).get("qml_format", False))


def check_qml_format(project_dir) -> List[str]:
    """Report QML that qmlformat would reformat, as a warning.

    qmlformat has no --check mode in 6.11: it writes in place or prints to stdout, so the
    check is to format to stdout and compare. A warning, never an error: formatting is not
    correctness, and `synqt check` still does not format anything, it only says what differs.

    Needs the project's own .qmlformat.ini and says so when there is none. Without -s,
    qmlformat falls back to a PER-USER settings file (~/.config/.qmlformat.ini), so the same
    QML would get a different answer on each machine and a third in CI.
    """
    qmlformat = qmlformat_path()
    if qmlformat is None:
        return ["warn: qmlformat not found; skipping the QML format check"]
    settings = Path(project_dir) / ".qmlformat.ini"
    if not settings.is_file():
        return ["warn: check.qml_format is on but the project has no .qmlformat.ini; "
                "skipping (without one qmlformat reads each machine's per-user settings, so "
                "the check would not be reproducible)"]
    # -s overrides the per-directory and per-user lookup, which is the whole point.
    unformatted: List[str] = []
    for qml in project_qml_files(project_dir):
        result = subprocess.run([qmlformat, "-s", str(settings), str(qml)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            continue  # a file qmlformat cannot parse is qmllint's finding to report, not ours
        if result.stdout != qml.read_text():
            unformatted.append(str(qml.relative_to(project_dir)))
    if not unformatted:
        return []
    return [f"warn: qmlformat would reformat {len(unformatted)} file(s): "
            f"{', '.join(unformatted)} (run: qmlformat -s .qmlformat.ini -i <file>)"]


def lint_qml(project_dir) -> List[str]:
    """Lint the project's QML with qmllint.

    Reads qmllint's OUTPUT, not its exit status: qmllint exits 0 for warnings, so a check
    that tests the status alone reports nothing no matter what it found. The fatal
    categories are elevated to errors and fail the check; everything else stays a warning,
    because qmllint cannot resolve the generated SynQt module here and would otherwise
    drown the real findings in import noise.
    """
    qmllint = qmllint_path()
    if qmllint is None:
        return ["warn: qmllint not found; skipping QML lint"]
    elevate: List[str] = []
    for category in _QML_FATAL_CATEGORIES:
        elevate += [f"--{category}", "error"]
    messages: List[str] = []
    for qml in project_qml_files(project_dir):
        result = subprocess.run([qmllint, *elevate, str(qml)],
                                capture_output=True, text=True)
        output = (result.stderr or "") + (result.stdout or "")
        for line in output.splitlines():
            if line.startswith("Error:") and any(f"[{c}]" in line
                                                 for c in _QML_FATAL_CATEGORIES):
                messages.append(f"error: qmllint {line[len('Error:'):].strip()}")
    return messages


def check_project(project_dir) -> Tuple[bool, List[str]]:
    """The full `synqt check`: topology validation + contract lint + loading lint + QML lint."""
    config_path = Path(project_dir) / "synqt.yaml"
    config = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    ok, messages = validate(config)
    contract_messages = lint_contracts(project_dir)
    loading_messages = lint_loading(project_dir)
    client_root_messages = lint_client_root(project_dir)
    messages += contract_messages
    messages += loading_messages
    qml_messages = lint_qml(project_dir)
    messages += client_root_messages
    messages += qml_messages
    if wants_qml_format_check(config):
        messages += check_qml_format(project_dir)
    ok = ok and not any(
        m.startswith("error:")
        for m in contract_messages + loading_messages + client_root_messages + qml_messages)
    if not ok:
        # validate() adds its "ok: topology valid" before the lints have run; printing it
        # above a list of errors reads as a pass. The lints get the last word.
        messages = [m for m in messages if not m.startswith("ok:")]
    return ok, messages
