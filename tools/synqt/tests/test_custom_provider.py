# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A `custom:` provider is compiled into the entity that selects it, with no CMake to edit.

`synqt add provider` writes providers/custom/<name>provider.cpp, and the registration macro
in it only runs if the file is linked into the entity. The root CMakeLists is regenerated on
every build, so a hand-added target_sources would not survive one; the selection itself has
to be what pulls the file in.
"""

import tempfile
from pathlib import Path

from synqt import addprovider, appgen


def _config(provider_name):
    entity = {"name": "database", "kind": "service", "blueprint": "persistence"}
    if provider_name is not None:
        entity["provider"] = {"name": provider_name}
    return {
        "project": {"name": "shop"},
        "entities": [{"name": "client", "kind": "client"},
                     {"name": "web", "kind": "service", "capability": "web_edge"},
                     entity],
    }


def test_a_custom_provider_selection_compiles_the_directory_in():
    cmake = appgen.render_root_cmakelists(_config("custom:SqlServer"), synqt_root="/synqt")
    assert "providers/custom/*.cpp" in cmake
    assert "target_sources(database PRIVATE ${SYNQT_CUSTOM_PROVIDERS_DATABASE})" in cmake
    # Re-globbed by the build, so a provider added after the first configure is picked up.
    assert "CONFIGURE_DEPENDS" in cmake


def test_a_bundled_provider_pulls_in_nothing():
    for name in (None, "sqlite", "postgres", "mysql"):
        cmake = appgen.render_root_cmakelists(_config(name), synqt_root="/synqt")
        assert "providers/custom" not in cmake, name


def test_the_scaffolded_file_lands_where_the_glob_looks():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        addprovider.scaffold(root, "SqlServer", "persistence")
        written = root / "providers" / "custom" / "sqlserverprovider.cpp"
        assert written.is_file()
        source = written.read_text()
        assert 'SYNQT_REGISTER_PERSISTENCE_PROVIDER("SqlServer", SqlServerProvider)' in source
