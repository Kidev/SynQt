# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The platform deployment step for the native desktop client, run only when asked.

`synqt build --client desktop` does not deploy (docs/desktop.md): signing, entitlements,
notarization and installer format are the project's choice. The build makes the step
possible; on macOS that is why the client is an .app bundle, the only input macdeployqt
takes. `--deploy` runs the step. It never signs unless told to.
"""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set

from . import appmodel


class DeployError(Exception):
    """The deploy step could not run. Carries a message meant for the CLI's output."""


# Where a kit keeps its executables. `qmlimportscanner` is in libexec, not bin.
_TOOL_DIRS = ("bin", "libexec")


def _tool(host_qt: Optional[str], name: str) -> Path:
    if not host_qt:
        raise DeployError(
            f"--deploy needs the host Qt kit to find {name}, and no host Qt was resolved. "
            "Run `synqt doctor` to see what the toolchain resolver found.")
    for directory in _TOOL_DIRS:
        for candidate in (Path(host_qt) / directory / name,
                          Path(host_qt) / directory / f"{name}.exe"):
            if candidate.exists():
                return candidate
    searched = " or ".join(f"{host_qt}/{directory}" for directory in _TOOL_DIRS)
    raise DeployError(
        f"--deploy could not find {name} in {searched}. It ships with the desktop Qt kit; "
        "a kit installed without it cannot deploy.")


def _run(command: List[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        # The tool's own diagnosis: macdeployqt names the plugin or framework it could not
        # resolve.
        detail = (result.stderr or result.stdout or "").strip().splitlines()
        tail = "\n    ".join(detail[-6:]) if detail else "(no output)"
        raise DeployError(f"{Path(command[0]).name} failed:\n    {tail}")
    return result.stdout


# What an unsigned binary costs, per platform: refused by Gatekeeper on macOS, a SmartScreen
# warning on Windows, nothing on Linux (no binary code signing).
REQUIRED = "required"
RECOMMENDED = "recommended"
NOT_APPLICABLE = "not-applicable"

SIGNING_REQUIREMENT = {
    "macos": REQUIRED,
    "windows": RECOMMENDED,
    "linux": NOT_APPLICABLE,
}

_UNSIGNED_CONSEQUENCE = {
    "macos": ("Gatekeeper refuses an unsigned app on every machine but the one that built it, "
              "so an unsigned macOS build is for local use only."),
    "windows": ("An unsigned .exe runs, but SmartScreen shows an unrecognised-publisher "
                "warning to everyone who downloads it."),
    "linux": ("Linux has no binary code signing, so an unsigned tree is normal and fully "
              "distributable. Signing happens at the package layer (a GPG-signed AppImage, "
              "or your distribution's repository)."),
}


def signing_consequence(platform: str) -> str:
    """One sentence on what shipping this platform unsigned means."""
    return _UNSIGNED_CONSEQUENCE.get(platform, _UNSIGNED_CONSEQUENCE["linux"])


def check_signing_choice(platform: str, sign: Optional[str], unsigned: bool) -> None:
    """Reject a --deploy that has not said what it does about signing (`--sign <identity>` or
    `--unsigned`).
    """
    if sign and unsigned:
        raise DeployError("--sign and --unsigned contradict each other; pass one.")
    if not sign and not unsigned:
        lead = {
            "macos": "On macOS a distributable build must be signed.",
            "windows": "On Windows a distributable build should be signed.",
            "linux": "On Linux there is no binary signing to do.",
        }.get(platform, "What signing means here depends on the platform.")
        # Offer only an option this platform accepts: never --sign on Linux.
        remedy = ("Pass --unsigned to acknowledge that, which is all there is to say here."
                  if SIGNING_REQUIREMENT.get(platform) == NOT_APPLICABLE
                  else "Pass --sign <identity> to sign it, or --unsigned to accept that "
                       "it is not.")
        raise DeployError(
            f"--deploy needs to know what to do about signing. {lead}\n"
            f"       {signing_consequence(platform)}\n"
            f"       {remedy}")
    if sign and SIGNING_REQUIREMENT.get(platform) == NOT_APPLICABLE:
        raise DeployError(
            "--sign has nothing to do on Linux: there is no binary code signing here.\n"
            f"       {signing_consequence('linux')}\n"
            "       Deploy with --unsigned, then sign the package you build from the tree.")


def deploy_client(root: Path, name: str, out: Path, resolved: Dict[str, Any],
                  platform: str, *, sign: Optional[str] = None) -> str:
    """Deploy the installed desktop client in `out`. Returns a one-line note for the summary."""
    host_qt = resolved.get("host_qt")
    if platform == "macos":
        app = out / f"{name}.app"
        if not app.is_dir():
            raise DeployError(f"--deploy found no app bundle at {app}.")
        command = [str(_tool(host_qt, "macdeployqt")), str(app), f"-qmldir={root}"]
        if sign:
            # macdeployqt's own -codesign, which signs nested frameworks and plugins before
            # the bundle. `codesign --deep` is not a substitute.
            command.append(f"-codesign={sign}")
        _run(command)
        if sign:
            return (f"deployed and signed {app.name} as {sign!r}; notarize with "
                    "`xcrun notarytool submit` before distributing")
        return f"deployed {app.name} with macdeployqt, UNSIGNED (local use only)"
    if platform == "windows":
        exe = out / f"{name}.exe"
        if not exe.is_file():
            raise DeployError(f"--deploy found no executable at {exe}.")
        _run([str(_tool(host_qt, "windeployqt")), "--qmldir", str(root), str(exe)])
        if sign:
            # The identity is the certificate subject name (signtool /n). Timestamped, so
            # the signature outlives the certificate.
            _run(["signtool", "sign", "/fd", "sha256", "/n", sign,
                  "/tr", "http://timestamp.digicert.com", "/td", "sha256", str(exe)])
            return f"deployed and signed {exe.name} as {sign!r}"
        return f"deployed {exe.name} with windeployqt, UNSIGNED (SmartScreen will warn)"
    return _deploy_linux(root, name, out, host_qt, _client_dir(root, resolved))


def _dynamic_needs(path: Path) -> List[str]:
    """The DT_NEEDED sonames of an ELF file, read from the file itself.

    Not `ldd`, which reports what resolved on this machine (possibly a system Qt), prints
    "not found" without a soname, and is missing on musl. Returns an empty list for a
    non-ELF file, so callers can pass every file in a directory.
    """
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return []
    is64 = data[4] == 2
    little = data[5] == 1
    prefix = "<" if little else ">"

    def read(fmt: str, offset: int):
        size = struct.calcsize(prefix + fmt)
        if offset < 0 or offset + size > len(data):
            return None
        return struct.unpack_from(prefix + fmt, data, offset)

    # e_phoff, e_phentsize and e_phnum are not adjacent, so each is read at its own offset:
    # 32/54/56 on ELF64, 28/42/44 on ELF32.
    offsets = (32, 54, 56) if is64 else (28, 42, 44)
    header_offset = read("Q" if is64 else "I", offsets[0])
    header_size = read("H", offsets[1])
    header_count = read("H", offsets[2])
    if header_offset is None or header_size is None or header_count is None:
        return []
    program_offset = header_offset[0]
    entry_size = header_size[0]
    entry_count = header_count[0]

    # PT_LOAD maps virtual addresses to file offsets for DT_STRTAB; PT_DYNAMIC is the table.
    loads: List[tuple] = []
    dynamic: Optional[tuple] = None
    for index in range(entry_count):
        base = program_offset + (index * entry_size)
        if is64:
            fields = read("IIQQQQQQ", base)
            if fields is None:
                continue
            kind, _flags, offset, vaddr, _paddr, filesz, _memsz, _align = fields
        else:
            fields = read("IIIIIIII", base)
            if fields is None:
                continue
            kind, offset, vaddr, _paddr, filesz, _memsz, _flags, _align = fields
        if kind == 1: # PT_LOAD
            loads.append((vaddr, offset, filesz))
        elif kind == 2: # PT_DYNAMIC
            dynamic = (offset, filesz)
    if dynamic is None:
        return []

    def to_offset(address: int) -> Optional[int]:
        for start, file_offset, length in loads:
            if start <= address < start + length:
                return file_offset + (address - start)
        return None

    tag_format = "qQ" if is64 else "iI"
    tag_size = struct.calcsize(prefix + tag_format)
    needed: List[int] = []
    strtab: Optional[int] = None
    position = dynamic[0]
    while position + tag_size <= dynamic[0] + dynamic[1]:
        entry = read(tag_format, position)
        if entry is None:
            break
        tag, value = entry
        if tag == 0: # DT_NULL
            break
        if tag == 1: # DT_NEEDED, an offset into the string table
            needed.append(value)
        elif tag == 5: # DT_STRTAB
            strtab = value
        position += tag_size
    if strtab is None:
        return []
    table = to_offset(strtab)
    if table is None:
        return []

    sonames: List[str] = []
    for offset in needed:
        start = table + offset
        end = data.find(b"\0", start)
        if start < len(data) and end != -1:
            sonames.append(data[start:end].decode("utf-8", "replace"))
    return sonames


def _library_closure(roots: Iterable[Path], kit: Path) -> Dict[str, Path]:
    """Every library in `kit` reachable from `roots`, transitively, keyed by soname.
    Transitive, because plugins and QML modules loaded at run time bring their own
    dependencies.
    """
    lib_dir = kit / "lib"
    found: Dict[str, Path] = {}
    pending: List[Path] = list(roots)
    visited: Set[Path] = set()
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        for soname in _dynamic_needs(current):
            if soname in found:
                continue
            candidate = lib_dir / soname
            if not candidate.exists():
                continue  # not the kit's: glibc, libX11 and friends are the host's to provide
            resolved = candidate.resolve()
            found[soname] = resolved
            pending.append(resolved)
    return found


def _qml_modules(root: Path, kit: Path) -> List[str]:
    """The QML modules the application imports, as paths relative to the kit qml/.

    Asked of the kit's `qmlimportscanner`, as windeployqt does. It includes every Controls
    style (chosen at run time) and each style's `.impl` companion.
    """
    scanner = _tool(str(kit), "qmlimportscanner")
    result = subprocess.run(
        [str(scanner), "-rootPath", str(root), "-importPath", str(kit / "qml")],
        capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip().splitlines()
        raise DeployError("qmlimportscanner failed, so which QML modules the client needs is "
                          "unknown and deploying would guess:\n    "
                          + ("\n    ".join(detail[-4:]) if detail else "(no output)"))
    try:
        entries = json.loads(result.stdout or "[]")
    except ValueError as error:
        raise DeployError(f"qmlimportscanner produced output that is not JSON: {error}")

    modules: List[str] = []
    for entry in entries:
        relative = entry.get("relativePath")
        path = entry.get("path")
        # A module with no path is compiled into the binary resources (the client `SynQt`
        # module).
        if not relative or not path:
            continue
        if (kit / "qml" / relative).is_dir():
            modules.append(relative)
    return modules


def _copy_module(source: Path, destination: Path) -> None:
    """Copy one QML module directory without the modules nested inside it.

    `QtQuick` contains `Controls`, `VirtualKeyboard` and more; each imported one is its own
    scanner entry. Other subdirectories (an Imagine style's `images/`) are module data and
    are copied. The rule is by qmldir, not by depth.
    """
    destination.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.is_dir():
            if (entry / "qmldir").is_file():
                continue
            shutil.copytree(entry, destination / entry.name, symlinks=True, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, destination / entry.name)


# The plugin directories each linked Qt module loads at run time. No dependency walk finds
# them; windeployqt keeps the same table. Only directories the kit has are copied.
_PLUGIN_CLASSES = {
    "Gui": ("platforms", "platformthemes", "platforminputcontexts", "imageformats",
            "iconengines", "generic", "xcbglintegrations", "egldeviceintegrations",
            "wayland-shell-integration", "wayland-graphics-integration-client",
            "wayland-decoration-client"),
    "Network": ("tls", "networkinformation"),
    "Widgets": ("styles",),
    "Sql": ("sqldrivers",),
    "Quick": ("scenegraph",),
    "Multimedia": ("multimedia",),
    "PrintSupport": ("printsupport",),
    "TextToSpeech": ("texttospeech",),
}


def _plugin_dirs(reachable: Iterable[str], kit: Path) -> List[str]:
    """The plugin directories this client can load, from the Qt modules reachable from it, not
    only those the executable links.
    """
    linked = {soname[len("libQt6"):].split(".so")[0]
              for soname in reachable if soname.startswith("libQt6")}
    directories: List[str] = []
    for module, classes in _PLUGIN_CLASSES.items():
        if module not in linked:
            continue
        for name in classes:
            if (kit / "plugins" / name).is_dir() and name not in directories:
                directories.append(name)
    return directories


def _client_dir(root: Path, resolved: Dict[str, Any]) -> Path:
    """The client entity's own directory, or the project root when it has none yet."""
    client = appmodel.client_entity(resolved)
    folder = root / appmodel.entity_dir(client) if client else root
    return folder if folder.is_dir() else root


def _deploy_linux(root: Path, name: str, out: Path, host_qt: Optional[str],
                  qml_root: Path) -> str:
    """The portable layout: Qt libraries and QML modules beside the binary, plus a launcher.

    Asks `qmlimportscanner` for the imported QML modules, adds the plugin directories their
    Qt modules load, and ships the transitive library closure of all of it. System libraries
    (glibc, libX11) are left to the host. The closure is verified before returning, because
    a gap only shows on a machine without Qt.
    """
    binary = out / name
    if not binary.is_file():
        raise DeployError(f"--deploy found no executable at {binary}.")
    if not host_qt:
        raise DeployError("--deploy needs the host Qt kit to know which libraries are Qt's.")
    kit = Path(host_qt).resolve()

    # Replaced, not merged, so nothing from a previous kit remains.
    for stale in ("lib", "qml", "plugins"):
        shutil.rmtree(out / stale, ignore_errors=True)

    # Scan the client entity directory only, not the project root.
    scan_root = qml_root

    shipped: List[Path] = [binary]
    modules = _qml_modules(scan_root, kit)
    for relative in modules:
        destination = out / "qml" / relative
        _copy_module(kit / "qml" / relative, destination)
        shipped += sorted(destination.glob("*.so"))

    # Two passes: the plugin directories follow from the reachable Qt modules, and the
    # plugins then add libraries of their own.
    plugin_dirs = _plugin_dirs(_library_closure(shipped, kit), kit)
    for directory in plugin_dirs:
        destination = out / "plugins" / directory
        shutil.copytree(kit / "plugins" / directory, destination,
                        symlinks=True, dirs_exist_ok=True)
        shipped += sorted(destination.rglob("*.so"))

    lib_dir = out / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    libraries = _library_closure(shipped, kit)
    for soname, source in libraries.items():
        # Copied under the soname, following the symlink (libQt6Core.so.6 ->
        # libQt6Core.so.6.12.0), since the loader asks for the DT_NEEDED name.
        shutil.copy2(source, lib_dir / soname, follow_symlinks=True)

    _verify_closure(shipped, lib_dir, kit)

    launcher = out / f"{name}.sh"
    launcher.write_text(
        "#!/usr/bin/env bash\n"
        "# Generated by `synqt build --deploy`. Runs the client against the Qt shipped\n"
        "# beside it rather than whatever the host happens to have installed.\n"
        'here="$(cd "$(dirname "$0")" && pwd)"\n'
        'export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        'export QML_IMPORT_PATH="$here/qml${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}"\n'
        'export QT_PLUGIN_PATH="$here/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"\n'
        f'exec "$here/{name}" "$@"\n')
    launcher.chmod(launcher.stat().st_mode | 0o111)
    # No "UNSIGNED" warning on Linux, where unsigned is normal.
    return (f"deployed {name} as a portable layout ({len(libraries)} Qt libraries, "
            f"{len(modules)} QML modules, {len(plugin_dirs)} plugin directories); "
            f"launch through {launcher.name}")


def _verify_closure(shipped: Iterable[Path], lib_dir: Path, kit: Path) -> None:
    """Refuse to report success when something in the tree still needs the kit. The failure is
    invisible on any machine that has Qt installed.
    """
    libraries = sorted(lib_dir.iterdir()) if lib_dir.is_dir() else []
    present = {entry.name for entry in libraries}
    missing: Dict[str, str] = {}
    # The shipped libraries are checked too, not only their dependents.
    for path in list(shipped) + libraries:
        for soname in _dynamic_needs(path):
            if soname in present or not (kit / "lib" / soname).exists():
                continue
            missing.setdefault(soname, path.name)
    if missing:
        detail = ", ".join(f"{soname} (needed by {by})" for soname, by in sorted(missing.items()))
        raise DeployError(
            "--deploy did not ship every library the tree needs, so it would fail to start on "
            f"a machine without Qt installed: {detail}. This is a bug in SynQt's deploy step.")
