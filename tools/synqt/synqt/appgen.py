# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the buildable app from the declared topology: the multi-binary root
``CMakeLists.txt`` and one ``main.cpp`` per entity.

This is the piece that turns ``synqt.yaml`` into something the pinned toolchain can
compile, and it is the entry point the rest of the CLI calls: :func:`generate` writes
everything an entity needs to build. The work is split by what it emits, because the
four kinds of output share almost nothing but the topology they read:

- :mod:`synqt.appmodel` reads the topology (entities, connect points, scopes, routes,
  views, the client's QML files) and refuses one it cannot read. Nothing there emits.
- :mod:`synqt.cmakegen` renders the root ``CMakeLists.txt``.
- :mod:`synqt.maingen` renders the client, edge and service ``main.cpp``.
- :mod:`synqt.clientshell` renders what the browser loads before the client does:
  ``index.html``, ``synqt-boot.js``, the shell cache worker, and the dev reload hook.
- :mod:`synqt.authentity` renders the Source QML the auth entity needs when
  ``identity.provider_entity`` promotes identity out of the edge.

Nothing is re-exported here: a caller that wants one renderer names the module that
owns it, so the split stays load bearing rather than a layer behind one facade. A
topology this generator cannot read raises :class:`synqt.appmodel.AppGenError`.

Generation is deterministic string rendering (unit-testable without a compiler); the
actual compilation runs through the CMake presets in :mod:`synqt.build`.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel, authentity, cmakegen, maingen, writer


def generate(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
             synqt_root: os.PathLike[str] | str | None = None) -> List[str]:
    """Write the root CMakeLists and one main.cpp per entity.

    Returns every path this generator owns, whether or not this run had to touch it: each
    file is written only when its content changed (see :mod:`synqt.writer`), so the return
    describes the app's generated surface rather than what the filesystem did. That
    distinction is the point; a caller wanting the second one would be asking the wrong
    question, since an unchanged file is exactly what makes a rebuild free.
    """
    root = Path(project_dir)
    synqt_root = Path(synqt_root) if synqt_root else appmodel.framework_root()
    # `identity.provider_entity` implies two mesh links (the auth entity owns identity and
    # sessions; every edge consumes them). Expanded once here so the CMake, every main.cpp
    # and the Source QML below all see the same topology.
    config = appmodel.with_auth_connect_points(config)
    written: List[str] = []

    cmake_path = root / "CMakeLists.txt"
    writer.write_if_changed(cmake_path,
                            cmakegen.render_root_cmakelists(config, synqt_root, root))
    written.append("CMakeLists.txt")

    for entity in appmodel.entities(config):
        name = entity.get("name")
        if not name:
            continue
        entity_dir = root / name
        entity_dir.mkdir(parents=True, exist_ok=True)
        singletons = appmodel.discover_singletons(entity_dir)
        if entity.get("kind") == "client":
            # The same QML module URI the client target is configured with in
            # render_root_cmakelists (qt_add_qml_module URI ...), so a compiled-in route's
            # qrc URL actually matches where qmlcachegen puts the view.
            uri = appmodel.qml_uri(config.get("project", {}).get("name", "app"))
            source = maingen.render_client_main(config, uri)
        elif appmodel.is_edge(entity):
            source = maingen.render_edge_main(config, entity, singletons)
        else:
            source = maingen.render_service_main(config, entity, singletons)
        writer.write_if_changed(entity_dir / "main.cpp", source)
        written.append(f"{name}/main.cpp")

        # The auth entity's Sources: one bridge per framework connect point it owns, from
        # the connect point's own `server:` path, so the file and the topology cannot
        # disagree about where it is.
        for connect_point in appmodel.owned_by(config, name):
            if not appmodel.is_framework_point(connect_point):
                continue
            relative = connect_point.get("server")
            source_qml = authentity.render_source_qml(connect_point.get("contract", ""))
            writer.write_if_changed(root / relative, source_qml)
            written.append(relative)

    return written
