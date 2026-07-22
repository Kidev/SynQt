# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The generated client carries a component URL per route, and the router base."""

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
