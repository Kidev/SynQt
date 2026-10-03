# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Service entities written in C++.

``cpp: true`` on a ``type: service`` entity makes its Source a C++ class. The entity folder
holds ``lobby.h`` and ``lobby.cpp`` (class ``Lobby``), and may still hold ``Lobby.qml``,
rooted at that class, for whatever is easier to write in QML::

    service/lobby/lobby.h        class Lobby : public SynQt::Service
    service/lobby/lobby.cpp
    service/lobby/Lobby.qml      optional: Lobby { ... }
    service/lobby/CMakeLists.txt optional: libraries, extra sources

``SynQt::Service`` comes from ``#include <SynQtService>``, a header generated per entity
under ``generated/``. It is a class named after the entity (``LobbyService``) deriving from
the generated Source helper, so the bounds, scope gates, tracing, fronts and shared mirrors
all stay in front of the author's code. Each slot is implemented by overriding the helper's
implementation overload, which takes the Caller first.

Without ``Lobby.qml``, the build writes one into the ``generated/`` mirror (``Lobby {}``), so
the runtime loads every Source the same way.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List, NamedTuple, Optional

from . import appmodel

#: The entity key that selects C++.
CPP_KEY = "cpp"

#: The files an entity folder compiles in. Anything else a build needs is added by the
#: folder's own CMakeLists.txt.
CPP_SUFFIXES = (".h", ".cpp")

#: The header an author includes, and the one it includes (AUTOMOC reads only a file with a
#: header suffix).
INCLUDE_HEADER = "SynQtService"
SERVICE_HEADER = "synqtservice.h"

#: The optional build file in the entity folder.
ENTITY_CMAKE = "CMakeLists.txt"

#: A name that can be a C++ class, and so a C++ entity.
CPP_IDENTIFIER = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")

_HEADER_CPP = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
               "// SPDX-License-Identifier: Apache-2.0\n")


def is_cpp(entity: Dict[str, Any]) -> bool:
    """Whether this entity's Source is written in C++."""
    return entity.get(CPP_KEY) is True


def class_name(entity: Dict[str, Any]) -> str:
    """The author's class: the entity name, capitalized, as its QML type is."""
    return appmodel.accessor_name(str(entity.get("name") or ""))


def file_stem(entity: Dict[str, Any]) -> str:
    """The class name in lower case, as Qt names a class's files."""
    return class_name(entity).lower()


def header_path(entity: Dict[str, Any]) -> str:
    """The author's header, project-relative."""
    return f"{appmodel.entity_dir(entity)}/{file_stem(entity)}.h"


def source_path(entity: Dict[str, Any]) -> str:
    """The author's source file, project-relative."""
    return f"{appmodel.entity_dir(entity)}/{file_stem(entity)}.cpp"


def entity_cmake_path(entity: Dict[str, Any]) -> str:
    """The entity's own optional CMakeLists.txt, project-relative."""
    return f"{appmodel.entity_dir(entity)}/{ENTITY_CMAKE}"


def generated_folder(entity: Dict[str, Any]) -> str:
    """Where this entity's generated C++ lands, project-relative."""
    return f"{appmodel.GENERATED_DIR}/{appmodel.entity_dir(entity)}"


def cpp_files(project_dir: os.PathLike[str] | str, entity: Dict[str, Any]) -> List[str]:
    """Every `.h` and `.cpp` directly in the entity folder, project-relative and sorted.
    Subfolders are the entity's own CMakeLists.txt to add.
    """
    root = Path(project_dir)
    folder = root / appmodel.entity_dir(entity)
    if not folder.is_dir():
        return []
    return sorted(path.relative_to(root).as_posix() for path in folder.iterdir()
                  if path.is_file() and path.suffix in CPP_SUFFIXES)


def owned_point(config: Dict[str, Any], entity: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """The application connect point this entity owns, if any."""
    owned = appmodel.app_points(appmodel.owned_by(config, str(entity.get("name") or "")))
    return owned[0] if owned else None


#: Comments, so a commented-out override is not read as one.
_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

#: A slot's implementation overload, declared with `override`: its name, then a first
#: parameter that is a pointer to a Caller.
_OVERRIDE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*(?:SynQt\s*::\s*)?Caller\s*\*[^;{}]*?\)"
    r"\s*(?:const\s*)?override\b")


def implemented_slots(project_dir: os.PathLike[str] | str,
                      entity: Dict[str, Any]) -> Dict[str, str]:
    """The slots the entity's headers override, each with the header that does. Read from the
    text: the compiler checks every signature, so a name is all `synqt check` needs.
    """
    root = Path(project_dir)
    found: Dict[str, str] = {}
    for relative in cpp_files(root, entity):
        if not relative.endswith(".h"):
            continue
        try:
            text = (root / relative).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in _OVERRIDE.finditer(_COMMENT.sub(" ", text)):
            found.setdefault(match.group(1), relative)
    return found


class Accessor(NamedTuple):
    """One thing the entity's QML sees by name, offered to its C++ as a typed method."""

    method: str
    cpp_type: str
    include: str
    context_name: str


def accessors(config: Dict[str, Any], entity: Dict[str, Any]) -> List[Accessor]:
    """What `SynQt::Service` offers: `log()` always, `http()` and `api()` where the network
    block installs them, and one method per entity this one consumes, named after it.
    """
    found = [Accessor("log", "SynQt::Log", "log.h", "Log")]
    if appmodel.declares_outbound(entity):
        found.append(Accessor("http", "SynQt::Http", "http.h", "Http"))
    if appmodel.serves_inbound(entity):
        found.append(Accessor("api", "SynQt::Api", "api.h", "Api"))
    name = str(entity.get("name") or "")
    for point in appmodel.app_points(appmodel.mesh_consumed(config, name)):
        owner = str(point.get("owner") or "")
        contract = appmodel.contract_of(point)
        if not CPP_IDENTIFIER.match(owner) or not contract:
            continue
        found.append(Accessor(owner, f"{contract}Consumer", f"{contract.lower()}_consumer.h",
                              appmodel.accessor_name(owner)))
    return found


def render_include_header(entity: Dict[str, Any]) -> str:
    """`<SynQtService>`: the header an author includes, in the Qt style of a class header."""
    return "\n".join([_HEADER_CPP.rstrip("\n"),
                      f"// Generated by `synqt build` for the {entity.get('name')} entity. "
                      "Do not edit.",
                      "",
                      f'#include "{SERVICE_HEADER}"',
                      ""])


def render_service_header(config: Dict[str, Any], entity: Dict[str, Any]) -> str:
    """`synqtservice.h`: the class `SynQt::Service` names in this entity.

    The caller must have checked that the entity owns a point (`synqt check` refuses a C++
    entity that owns none).
    """
    name = str(entity.get("name") or "")
    point = owned_point(config, entity)
    contract = appmodel.contract_of(point) if point else class_name(entity)
    base = f"{contract}SourceBase"
    generated = f"{class_name(entity)}Service"
    found = accessors(config, entity)
    runtime = sorted({accessor.include for accessor in found
                      if not accessor.include.endswith("_consumer.h")})
    consumed = [accessor.include for accessor in found
                if accessor.include.endswith("_consumer.h")]

    lines = [_HEADER_CPP.rstrip("\n"),
             f"// Generated by `synqt build` for the {name} entity. Do not edit: it is rewritten",
             "// from synqt.yaml on every build.",
             "",
             "#pragma once",
             "",
             f'#include "{contract.lower()}_sourcehelper.h"']
    lines += [f'#include "{include}"' for include in consumed]
    lines.append("")
    lines += ["#include <caller.h>"] + [f"#include <{include}>" for include in runtime]
    lines += ["",
              "#include <QObject>",
              "#include <QQmlContext>",
              "#include <QQmlEngine>",
              "#include <QQmlParserStatus>",
              "#include <QString>",
              "#include <QVariant>",
              "",
              f"/// What `SynQt::Service` is in the {name} entity: the {contract} Source, with",
              "/// what the entity's QML sees by name offered as typed methods.",
              "///",
              "/// Every slot is declared twice by the base. The wire slot checks bounds and",
              "/// scope and is final; the overload taking the Caller first is what a subclass",
              "/// overrides. Props, models and signals are the base's own: `setHeadline()`,",
              "/// `setCatalogue(rows)`, `emit restocked(...)`.",
              "///",
              "/// The accessors read the QML context the runtime builds the Source in, so they",
              "/// answer from componentComplete() on, never in the constructor.",
              f"class {generated} : public {base}, public QQmlParserStatus",
              "{",
              "    Q_OBJECT",
              "    Q_INTERFACES(QQmlParserStatus)",
              "",
              "public:",
              f"    explicit {generated}(QObject *parent = nullptr)",
              f"        : {base}{{parent}}",
              "    {",
              "    }",
              "",
              "    void classBegin() override {}",
              "    void componentComplete() override {}",
              "",
              "protected:"]
    for accessor in found:
        lines += [f"    {accessor.cpp_type} *{accessor.method}() const",
                  "    {",
                  f"        return synqtContextObject<{accessor.cpp_type}>("
                  f'QStringLiteral("{accessor.context_name}"));',
                  "    }"]
    lines += ["",
              "private:",
              "    template <typename T>",
              "    T *synqtContextObject(const QString &name) const",
              "    {",
              "        const QQmlContext *context{qmlContext(this)};",
              "        if (context == nullptr) {",
              "            return nullptr;",
              "        }",
              "        return qobject_cast<T *>(context->contextProperty(name).value<QObject *>());",
              "    }",
              "};",
              "",
              "namespace SynQt {",
              f"using Service = {generated};",
              "} // namespace SynQt",
              ""]
    return "\n".join(lines)


def render_stub_qml(entity: Dict[str, Any]) -> str:
    """The Source file for a C++ entity that has none: the class, with nothing added."""
    return "\n".join([
        _HEADER_CPP.rstrip("\n"),
        f"// Generated by `synqt build`: {appmodel.entity_dir(entity)} has no "
        f"{class_name(entity)}.qml, so the",
        "// entity is its C++ class alone. Do not edit.",
        "",
        "import SynQt",
        "",
        f"{class_name(entity)} {{}}",
        ""])


def _implementation_signatures(point: Dict[str, Any]) -> List[str]:
    """The implementation overload of each slot the point exports, as the generated base
    declares it (`void add(SynQt::Caller *caller, const QString &text)`).
    """
    from . import contractgen, designdoc

    designdoc._synqtc()
    from synqtc import emit, parser

    contract = appmodel.contract_of(point)
    syn = parser.parse_text(contractgen.contract_source(contract, point),
                            path=f"{contract}.syn", stem=contract)
    signatures: List[str] = []
    for declared in syn.contracts:
        for slot in declared.slots:
            signatures.append(emit._impl_signature(slot, syn.record_names, f"{contract}.syn"))
    return signatures


def render_header(entity: Dict[str, Any], point: Dict[str, Any]) -> str:
    """A starting `lobby.h`: the class, with one override per exported slot."""
    name = str(entity.get("name") or "")
    cls = class_name(entity)
    lines = [_HEADER_CPP.rstrip("\n"),
             "",
             "#pragma once",
             "",
             f"#include <{INCLUDE_HEADER}>",
             "",
             f"// The {name} entity. Each slot its connect point exports is answered by an override",
             "// below, once the generated base has checked the call's bounds and scope.",
             f"class {cls} : public SynQt::Service",
             "{",
             "    Q_OBJECT",
             "",
             "public:",
             f"    explicit {cls}(QObject *parent = nullptr);",
             "",
             "protected:",
             "    void componentComplete() override;"]
    lines += [f"    {signature} override;" for signature in _implementation_signatures(point)]
    lines += ["};", ""]
    return "\n".join(lines)


def render_source(entity: Dict[str, Any], point: Dict[str, Any]) -> str:
    """A starting `lobby.cpp`, matching :func:`render_header`."""
    cls = class_name(entity)
    lines = [_HEADER_CPP.rstrip("\n"),
             "",
             f'#include "{file_stem(entity)}.h"',
             "",
             f"{cls}::{cls}(QObject *parent)",
             "    : SynQt::Service{parent}",
             "{",
             "}",
             "",
             f"void {cls}::componentComplete()",
             "{",
             "    // The QML context is set from here on, so log() and each consumed entity answer.",
             "}"]
    for signature in _implementation_signatures(point):
        returned, rest = signature.split(" ", 1)
        method, params = rest.split("(", 1)
        names = [part.strip().split()[-1].lstrip("*&")
                 for part in params.rstrip(")").split(",") if part.strip()]
        lines += ["",
                  f"{returned} {cls}::{method}({params}",
                  "{",
                  "    // Authorize the caller here, before anything changes."]
        lines += [f"    Q_UNUSED({argument})" for argument in names]
        if returned != "void":
            lines.append("    return {};")
        lines.append("}")
    lines.append("")
    return "\n".join(lines)


def render_entity_cmake(entity: Dict[str, Any]) -> str:
    """A starting `CMakeLists.txt` for the entity folder."""
    name = str(entity.get("name") or "")
    return "\n".join([
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        f"# Added to the build of the {name} entity. Link a library with",
        f"# target_link_libraries({name} PRIVATE ...), and more Qt modules with",
        f"# synqt_entity_qt_modules({name} <Component> ...).",
        ""])


def write_scaffold(project_dir: os.PathLike[str] | str, entity: Dict[str, Any],
                   point: Dict[str, Any]) -> List[str]:
    """Write the starting header, source and CMakeLists.txt that do not exist yet. Returns the
    project-relative paths written.
    """
    root = Path(project_dir)
    written: List[str] = []
    for relative, render in ((header_path(entity), lambda: render_header(entity, point)),
                             (source_path(entity), lambda: render_source(entity, point)),
                             (entity_cmake_path(entity), lambda: render_entity_cmake(entity))):
        target = root / relative
        if target.exists():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(render(), encoding="utf-8")
        written.append(relative)
    return written


def write_generated(project_dir: os.PathLike[str] | str,
                    config: Dict[str, Any]) -> List[str]:
    """Write `<SynQtService>` and `synqtservice.h` for every C++ entity that owns a point.
    Returns the project-relative paths written or unchanged.
    """
    from . import writer

    root = Path(project_dir)
    written: List[str] = []
    for entity in appmodel.entities(config):
        if not is_cpp(entity) or owned_point(config, entity) is None:
            continue
        folder = generated_folder(entity)
        writer.write_if_changed(root / folder / INCLUDE_HEADER, render_include_header(entity))
        writer.write_if_changed(root / folder / SERVICE_HEADER,
                                render_service_header(config, entity))
        written += [f"{folder}/{INCLUDE_HEADER}", f"{folder}/{SERVICE_HEADER}"]
    return written
