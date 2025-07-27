# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The generated client carries a component URL per route, and the router base."""

import tempfile
from pathlib import Path

import pytest

from synqt import appgen


def test_route_carries_a_component_url():
    config = {
        "name": "shop",
        "routes": [{"path": "/", "view": "Home.qml"},
                   {"path": "/cart", "view": "Cart.qml"}],
    }
    source = appgen.render_client_main(config, uri="Shop")
    assert 'qrc:/qt/qml/Shop/Home.qml' in source
    assert 'qrc:/qt/qml/Shop/Cart.qml' in source


def test_view_name_without_extension_still_resolves():
    config = {"name": "shop", "routes": [{"path": "/", "view": "Main"}]}
    source = appgen.render_client_main(config, uri="Shop")
    assert 'qrc:/qt/qml/Shop/Main.qml' in source


def _client_cmake(routes):
    config = {
        "project": {"name": "shop"},
        "entities": [{"name": "client", "kind": "client"}],
        "routes": routes,
    }
    return appgen.render_root_cmakelists(config, synqt_root="/synqt")


def _client_project(files, routes=()):
    """A project on disk whose client entity holds `files` (relative path -> contents)."""
    root = Path(tempfile.mkdtemp())
    for name, text in files.items():
        path = root / "client" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    config = {
        "project": {"name": "shop"},
        "entities": [{"name": "client", "kind": "client"}],
        "routes": list(routes),
    }
    return appgen.render_root_cmakelists(config, synqt_root="/synqt", project_dir=root)


_ITEM = "import QtQuick\n\nItem {}\n"
_SINGLETON = "pragma Singleton\nimport QtQuick\n\nQtObject {}\n"


def test_a_view_written_with_a_leading_dot_slash_is_one_view_not_two():
    # './About.qml' and 'About.qml' are the same file, and the generator must spell it
    # one way: a literal './' would otherwise land in both the resource alias and the
    # compiled-in qrc:/qt/qml/Shop/./About.qml, which is a second entry for one file.
    source = appgen.render_client_main(
        {"name": "shop", "routes": [{"path": "/about", "view": "./About.qml"}]}, uri="Shop")
    assert "qrc:/qt/qml/Shop/About.qml" in source
    assert "/./" not in source

    cmake = _client_cmake([{"path": "/about", "view": "./About.qml"},
                           {"path": "/a", "view": "About"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS About.qml)") == 1


def test_a_view_in_a_subdirectory_keeps_its_subdirectory():
    # A view is named relative to the client entity's directory, so 'views/Home.qml' is
    # aliased into the module at that same relative path and the route's URL matches it.
    source = appgen.render_client_main(
        {"name": "shop", "routes": [{"path": "/", "view": "views/Home.qml"}]}, uri="Shop")
    assert "qrc:/qt/qml/Shop/views/Home.qml" in source

    cmake = _client_cmake([{"path": "/", "view": "views/Home.qml"}])
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/client/views/Home.qml"' in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS views/Home.qml)" in cmake


def test_a_route_with_no_view_is_refused_at_generation():
    # A route with no view used to default to Main.qml, which is the window: a Loader on
    # Router.pageComponent inside Main.qml would then load the window inside itself.
    # `synqt check` reports it earlier, but nothing makes `synqt build` run the check.
    with pytest.raises(appgen.AppGenError) as raised:
        appgen.render_client_main({"name": "shop", "routes": [{"path": "/admin"}]},
                                  uri="Shop")
    assert "/admin" in str(raised.value)
    assert "declares no view" in str(raised.value)

    with pytest.raises(appgen.AppGenError):
        _client_cmake([{"path": "/admin", "view": ""}])


def test_a_view_reaching_outside_the_client_directory_is_refused_at_generation():
    # The escape rule `synqt check` enforces has to hold here too: nothing makes
    # `synqt build` run the check, and a '../web/A.qml' view would otherwise land in the
    # resource alias and in qrc:/qt/qml/Shop/../web/A.qml, neither of which names a file.
    for view in ("../web/A.qml", "..\\web\\A.qml", "/etc/A.qml", "C:/x/B.qml",
                 "C:\\x\\B.qml"):
        with pytest.raises(appgen.AppGenError) as raised:
            appgen.render_client_main({"name": "shop",
                                       "routes": [{"path": "/a", "view": view}]}, uri="Shop")
        assert "absolute or parent path" in str(raised.value), view
        assert "/a" in str(raised.value), view
        with pytest.raises(appgen.AppGenError):
            _client_cmake([{"path": "/a", "view": view}])


def test_the_escape_predicate_takes_the_views_that_are_really_paths():
    # A legal POSIX filename that happens to start with a letter and a colon is a view,
    # not a Windows drive path; the separator after the colon is what tells them apart.
    for accepted in ("Home.qml", "./Home.qml", "views/Home.qml", "Home", "a:b.qml",
                     "views/a:b.qml"):
        assert not appgen.view_escapes_client_directory(accepted), accepted
    for rejected in ("../a.qml", "..\\a.qml", "/a.qml", "C:/a.qml", "C:\\a.qml",
                     "views/../../a.qml"):
        assert appgen.view_escapes_client_directory(rejected), rejected


def test_a_views_helper_components_are_compiled_in_too():
    # A view that instantiates a sibling Card.qml needs that file inside the same module,
    # or it fails to load exactly the way a view outside the module does.
    cmake = _client_project({"Main.qml": _ITEM, "Home.qml": _ITEM, "Card.qml": _ITEM,
                             "parts/Badge.qml": _ITEM},
                            routes=[{"path": "/", "view": "Home.qml"}])
    assert "PROPERTIES QT_RESOURCE_ALIAS Card.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS parts/Badge.qml)" in cmake
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/client/parts/Badge.qml"' in cmake


def test_a_singleton_is_marked_as_one():
    # Without QT_QML_SINGLETON_TYPE the module registers Theme.qml as an ordinary type
    # and a view reading `Theme.color` does not compile.
    cmake = _client_project({"Main.qml": _ITEM, "Theme.qml": _SINGLETON})
    assert "PROPERTIES QT_QML_SINGLETON_TYPE TRUE QT_RESOURCE_ALIAS Theme.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS Main.qml)" in cmake


def test_build_output_under_the_client_is_never_swept_in():
    # `synqt build` and a stray CMake run both leave copies of the QML under the entity;
    # compiling those back in would duplicate every type in the module.
    cmake = _client_project({"Main.qml": _ITEM, "build/Main.qml": _ITEM,
                             "generated/Gen.qml": _ITEM, ".cache/Old.qml": _ITEM})
    assert "build/Main.qml" not in cmake
    assert "generated/Gen.qml" not in cmake
    assert "Old.qml" not in cmake
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Main.qml)") == 1


def test_a_route_view_on_disk_is_listed_once():
    cmake = _client_project({"Main.qml": _ITEM, "Home.qml": _ITEM},
                            routes=[{"path": "/", "view": "Home"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Home.qml)") == 1


def test_without_a_project_directory_the_module_is_main_and_the_route_views():
    # A caller rendering CMake from a config alone (no app on disk) still gets exactly
    # what this generator has always emitted.
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"}])
    assert cmake.count("QT_RESOURCE_ALIAS") == 2


def test_every_route_view_is_in_the_clients_qml_module():
    # A view outside the QML module is outside the resource system, so the qrc URL the
    # route table carries resolves to nothing and the router reports Error.
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"},
                           {"path": "/cart", "view": "Cart.qml"}])
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/client/Home.qml"' in cmake
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/client/Cart.qml"' in cmake
    # Each file is listed by absolute path, so each needs the alias that puts it at the
    # module root: that is the half of the URL qrc:/qt/qml/Shop/Home.qml the route needs.
    assert "PROPERTIES QT_RESOURCE_ALIAS Home.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS Cart.qml)" in cmake


def test_a_view_named_without_its_extension_is_listed_as_a_file():
    cmake = _client_cmake([{"path": "/", "view": "Home"}])
    assert '"${CMAKE_CURRENT_SOURCE_DIR}/client/Home.qml"' in cmake


def test_a_view_is_listed_once_however_many_routes_name_it():
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"},
                           {"path": "/home", "view": "Home"}])
    assert cmake.count('"${CMAKE_CURRENT_SOURCE_DIR}/client/Home.qml"') == 2  # file + alias
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Home.qml)") == 1


def test_main_is_never_listed_twice():
    cmake = _client_cmake([{"path": "/", "view": "Main.qml"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Main.qml)") == 1


def test_a_project_with_no_routes_compiles_an_empty_table():
    # No manufactured "/" -> Main.qml route: that view is the window, so a Loader bound
    # to Router.pageComponent inside it would load the window again. With no table
    # pageComponent stays null and an app that does not route is untouched.
    source = appgen.render_client_main({"name": "shop"}, uri="Shop")
    assert "config.routes = {};" in source
    cmake = _client_cmake([])
    assert cmake.count("QT_RESOURCE_ALIAS") == 1


def test_router_base_defaults_to_root_and_is_configurable():
    plain = appgen.render_client_main({"name": "shop", "routes": []}, uri="Shop")
    assert 'config.routerBase = QStringLiteral("/")' in plain

    based = appgen.render_client_main(
        {"name": "shop", "routes": [], "router": {"base": "/shop"}}, uri="Shop")
    assert 'config.routerBase = QStringLiteral("/shop")' in based


def test_router_fallback_defaults_to_root_and_is_configurable():
    plain = appgen.render_client_main({"name": "shop", "routes": []}, uri="Shop")
    assert 'config.routerFallback = QStringLiteral("/")' in plain

    redirected = appgen.render_client_main(
        {"name": "shop", "routes": [], "router": {"fallback": "/home"}}, uri="Shop")
    assert 'config.routerFallback = QStringLiteral("/home")' in redirected
