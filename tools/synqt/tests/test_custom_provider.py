# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A `custom:` provider is compiled into the entity that selects it. The registration macro
runs only if the file is linked, and the generated CMake is rewritten every build, so the
selection pulls the file in.
"""

import tempfile
from pathlib import Path

import pytest

from synqt import addprovider, cmakegen


def _config(provider_name):
    entity = {"name": "database", "type": "relational"}
    if provider_name is not None:
        entity["provider"] = {"name": provider_name}
    return {
        "project": {"name": "shop"},
        "entities": [{"name": "client", "type": "client"},
                     {"name": "web", "type": "web_edge"},
                     entity],
    }


def test_a_custom_provider_selection_compiles_the_directory_in():
    cmake = cmakegen.render_root_cmakelists(_config("custom:SqlServer"), synqt_root="/synqt")
    assert "providers/custom/*.cpp" in cmake
    assert "target_sources(database PRIVATE ${SYNQT_CUSTOM_PROVIDERS_DATABASE})" in cmake
    # Re-globbed by the build, so a provider added after the first configure is picked up.
    assert "CONFIGURE_DEPENDS" in cmake


def test_a_bundled_provider_pulls_in_nothing():
    for name in (None, "sqlite", "postgres", "mysql"):
        cmake = cmakegen.render_root_cmakelists(_config(name), synqt_root="/synqt")
        assert "providers/custom" not in cmake, name


def test_the_scaffolded_file_lands_where_the_glob_looks():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        addprovider.scaffold(root, "SqlServer", "relational")
        written = root / "providers" / "custom" / "sqlserverprovider.cpp"
        assert written.is_file()
        source = written.read_text()
        assert 'SYNQT_REGISTER_PERSISTENCE_PROVIDER("SqlServer", SqlServerProvider)' in source


@pytest.mark.parametrize("name", ["../escaped", "my provider", "9Rows", ""])
def test_a_name_that_cannot_be_a_cpp_class_writes_nothing(tmp_path, name):
    # The name is the class `<Name>Provider` and the file `<name>provider.cpp`, so a path or
    # a space would write elsewhere or into C++ that does not compile.
    with pytest.raises(addprovider.AddProviderError):
        addprovider.scaffold(tmp_path, name, "cache")
    assert list(tmp_path.rglob("*.cpp")) == []
    assert not (tmp_path.parent / "escapedprovider.cpp").exists()
