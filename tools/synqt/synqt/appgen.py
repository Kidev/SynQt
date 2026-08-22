# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the buildable app from the declared topology into the project's ``generated/``
directory.

:func:`generate` is the entry point the CLI calls. The work is split by output:

- :mod:`synqt.appmodel` reads the topology and refuses what it cannot read.
- :mod:`synqt.cmakegen` renders the CMake.
- :mod:`synqt.maingen` renders the client, edge and service ``main.cpp``.
- :mod:`synqt.clientshell` renders ``index.html``, ``synqt-boot.js``, the shell cache worker
  and the dev reload hook.
- :mod:`synqt.authentity` renders the auth entity Source QML under
  ``identity.provider_entity``.
- :mod:`synqt.qmlrewrite` mirrors each entity QML into ``generated/``, the tree the engines
  load.

Nothing is re-exported here. Errors raise :class:`synqt.appmodel.AppGenError`. Rendering is
deterministic; compilation runs through the presets in :mod:`synqt.build`.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List

from . import (appmodel, authentity, cmakegen, contractgen, graphics as graphicsmod,
               maingen, qmlrewrite, scopegen, writer)


def generate(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
             synqt_root: os.PathLike[str] | str | None = None,
             dev_tools: bool = False) -> List[str]:
    """Write the CMake and one main.cpp per entity.

    Returns every path this generator owns, written or unchanged (files are written only
    when their content changes, see :mod:`synqt.writer`). `dev_tools` allows
    development-only code in the edge main; only `synqt dev` passes it.
    """
    root = Path(project_dir)
    # An entity name becomes a C++ string, a CMake target and a directory under
    # generated/, and generation runs before any check does.
    for entity in appmodel.entities(config):
        name = str(entity.get("name") or "")
        if not appmodel.is_valid_entity_name(name):
            raise appmodel.AppGenError(
                f"'{name[:80]}' is not usable as an entity name: a name starts with a "
                f"letter and is made of letters, digits, underscores and hyphens, up to "
                f"{appmodel.ENTITY_NAME_MAX} characters")
    synqt_root = Path(synqt_root) if synqt_root else appmodel.framework_root()
    # Expand the links `identity.provider_entity` implies once, for every output below.
    config = appmodel.with_auth_connect_points(config)
    # And the ingest link `monitoring.entity` implies.
    config = appmodel.with_monitoring_connect_points(config)
    # Resolved once for the client route table and the edge page list. `synqt check` reports
    # it (check.lint_graphics).
    config, _ = graphicsmod.resolve(config, root)
    written: List[str] = []
    # Everything lands here (appmodel.GENERATED_DIR).
    generated = appmodel.generated_dir(root)
    generated.mkdir(parents=True, exist_ok=True)

    # Contracts first; everything below points the compiler at them.
    written += contractgen.write_contracts(root, config)

    writer.write_if_changed(generated / appmodel.GENERATED_CMAKE,
                            cmakegen.render_root_cmakelists(config, synqt_root, root))
    written.append(f"{appmodel.GENERATED_DIR}/{appmodel.GENERATED_CMAKE}")

    # The project's own root CMake file, written once (see
    # cmakegen.render_project_cmakelists).
    project_cmake = root / "CMakeLists.txt"
    if not project_cmake.exists():
        writer.write_if_changed(project_cmake, cmakegen.render_project_cmakelists(config))
    written.append("CMakeLists.txt")

    # The test runner, in its own directory (see cmakegen._tests_cmake).
    if appmodel.test_qml_files(root):
        tests = generated / "tests"
        tests.mkdir(parents=True, exist_ok=True)
        writer.write_if_changed(tests / "tests_main.cpp", maingen.render_tests_main(config))
        written.append(f"{appmodel.GENERATED_DIR}/tests/tests_main.cpp")
        writer.write_if_changed(tests / "CMakeLists.txt",
                                cmakegen.render_tests_cmakelists(config))
        written.append(f"{appmodel.GENERATED_DIR}/tests/CMakeLists.txt")

    for entity in appmodel.entities(config):
        name = entity.get("name")
        if not name:
            continue
        entity_dir = root / appmodel.entity_dir(entity)
        entity_dir.mkdir(parents=True, exist_ok=True)
        # The main mirrors the entity folder under generated/.
        main_dir = generated / appmodel.entity_dir(entity)
        main_dir.mkdir(parents=True, exist_ok=True)
        singletons = appmodel.discover_singletons(entity_dir)
        if appmodel.is_client(entity):
            # The client module URI, as in render_root_cmakelists, so compiled route URLs
            # match.
            uri = appmodel.qml_uri_for(config, entity)
            source = maingen.render_client_main(config, uri, entity)
        elif appmodel.entity_type(entity) == "monitor":
            source = maingen.render_monitor_main(config, entity, singletons)
        elif appmodel.is_edge(entity):
            source = maingen.render_edge_main(config, entity, singletons, dev_tools)
        else:
            source = maingen.render_service_main(config, entity, singletons)
        writer.write_if_changed(main_dir / "main.cpp", source)
        written.append(f"{appmodel.GENERATED_DIR}/{appmodel.entity_dir(entity)}/main.cpp")

        # The auth entity Sources, one per framework point it owns, at the point `server:`
        # path.
        for connect_point in appmodel.owned_by(config, name):
            if not appmodel.is_framework_point(connect_point):
                continue
            relative = connect_point.get("server")
            source_qml = authentity.render_source_qml(connect_point.get("contract", ""))
            writer.write_if_changed(root / relative, source_qml)
            written.append(relative)

    # The scope enum, beside the mapping hook so the hook needs no import. Only for a
    # project with a hook.
    hook = appmodel.identity_mapping_hook(config)
    if hook:
        # Beside the mirrored hook under generated/, the tree the engine loads
        # (mirrored_path).
        relative = qmlrewrite.mirrored_path(scopegen.scope_qml_path(hook))
        scope_file = root / relative
        scope_file.parent.mkdir(parents=True, exist_ok=True)
        writer.write_if_changed(scope_file, scopegen.render_scope_qml(
            appmodel.scope_vocab(config)))
        written.append(relative)

    # Last: it mirrors the entity folders, including the auth Sources written above.
    written += qmlrewrite.write_entity_qml(root, config)

    return written
