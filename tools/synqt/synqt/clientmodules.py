# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The optional Qt modules a client links, read off what its QML imports.

Every client links `licenses.CLIENT_MODULES`. A 3D client imports `QtQuick3D`, maybe with
`QtQuick3D.Physics`. The static WebAssembly build links a QML plugin only when its CMake
package was found, so `cmakegen` writes the `find_package` and link line from this, and
`licenses` lists the same modules. There is no configuration key: the import is the
declaration. Both 3D modules are GPLv3-only under open-source Qt.
"""

from __future__ import annotations

import dataclasses
import os
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from synqt import appmodel, qmlscan


@dataclasses.dataclass(frozen=True)
class AddOn:
    """One optional module: the import that asks for it and what the build names it."""

    #: The QML module URI. An import of it, or of any module under it, asks for this one.
    uri: str
    #: The component `find_package(Qt6 COMPONENTS ...)` takes.
    component: str
    #: The imported target the client links.
    target: str
    #: What THIRD-PARTY-LICENSES calls it (a key of `licenses._MODULE_LICENSE`).
    notice: str
    #: Other add-ons this one needs, by `uri`. Physics is built on Quick3D.
    needs: tuple = ()
    #: The aqt archives for `synqt doctor`'s hint. Quick3D needs qtshadertools, and at 6.12
    #: Qt Quick Timeline, which the plugin scan reaches at configure time.
    archives: tuple = ()


#: In link order, the more specific URI first. Only modules proven to link statically under
#: the WebAssembly kits (examples/plaza builds both).
ADD_ONS = (
    AddOn("QtQuick3D.Physics", "Quick3DPhysics", "Qt6::Quick3DPhysics",
          "Qt Quick 3D Physics", needs=("QtQuick3D",), archives=("qtquick3dphysics",)),
    AddOn("QtQuick3D", "Quick3D", "Qt6::Quick3D", "Qt Quick 3D",
          archives=("qtquick3d", "qtquicktimeline", "qtshadertools")),
)


def _wants(uri: str, module: str) -> bool:
    return module == uri or module.startswith(uri + ".")


def for_imports(modules: Iterable[str]) -> List[AddOn]:
    """The add-ons a set of imported module URIs asks for, dependencies included, in `ADD_ONS`
    order.
    """
    wanted = set()
    for module in modules:
        for add_on in ADD_ONS:
            if _wants(add_on.uri, module):
                wanted.add(add_on.uri)
                wanted.update(add_on.needs)
                break
    return [add_on for add_on in ADD_ONS if add_on.uri in wanted]


def for_client(client_dir: Optional[Path], files: Iterable[str]) -> List[AddOn]:
    """The add-ons the client whose QML is `files` (relative to `client_dir`) asks for. A
    missing file is skipped; the build reports it.
    """
    if client_dir is None:
        return []
    modules: List[str] = []
    for name in files:
        path = Path(client_dir) / name
        if not path.is_file():
            continue
        modules += qmlscan.imported_modules(
            qmlscan.stripped(path.read_text(encoding="utf-8", errors="replace")))
    return for_imports(modules)


def for_project(config: Dict[str, Any],
                project_dir: os.PathLike[str] | str) -> List[AddOn]:
    """Every add-on any client of this project asks for, which is what its kits must carry."""
    root = Path(project_dir)
    wanted: List[str] = []
    for entity in appmodel.entities(config):
        if not appmodel.is_client(entity):
            continue
        client_dir = root / appmodel.entity_dir(entity)
        files = appmodel.client_qml_files(config, client_dir, entity)
        wanted += [add_on.uri for add_on in for_client(client_dir, files)]
    return [add_on for add_on in ADD_ONS if add_on.uri in wanted]
