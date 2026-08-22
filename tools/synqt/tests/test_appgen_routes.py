# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The generated client carries a component URL per route, and the router base."""

import tempfile
from pathlib import Path

import pytest

from synqt import appmodel, cmakegen, maingen


def test_route_carries_a_component_url():
    config = {
        "name": "shop",
        "routes": [{"path": "/", "view": "Home.qml"},
                   {"path": "/cart", "view": "Cart.qml"}],
    }
    source = maingen.render_client_main(config, uri="Shop")
    assert 'qrc:/qt/qml/Shop/Home.qml' in source
    assert 'qrc:/qt/qml/Shop/Cart.qml' in source


def test_the_client_carries_its_tab_nonce_onto_the_sync_url():
    # The browser sends every host cookie on the upgrade, so the per-tab nonce must be on it
    # too or the socket reads the shared session.
    source = maingen.render_client_main({"name": "shop"}, uri="Shop")
    assert "QString tabNonce()" in source
    assert 'query.addQueryItem(QStringLiteral("s"), nonce)' in source
    # And it is read from the page the browser is on, not invented.
    assert 'location["search"]' in source


def test_view_name_without_extension_still_resolves():
    config = {"name": "shop", "routes": [{"path": "/", "view": "Main"}]}
    source = maingen.render_client_main(config, uri="Shop")
    assert 'qrc:/qt/qml/Shop/Main.qml' in source


def _client_cmake(routes):
    config = {
        "project": {"name": "shop"},
        "entities": [{"name": "app", "type": "client"}],
        "routes": routes,
    }
    return cmakegen.render_root_cmakelists(config, synqt_root="/synqt")


def _client_project(files, routes=()):
    """A project on disk whose client entity holds `files` (relative path -> contents)."""
    root = Path(tempfile.mkdtemp())
    for name, text in files.items():
        path = root / "client" / "app" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    config = {
        "project": {"name": "shop"},
        "entities": [{"name": "app", "type": "client"}],
        "routes": list(routes),
    }
    return cmakegen.render_root_cmakelists(config, synqt_root="/synqt", project_dir=root)


_ITEM = "import QtQuick\n\nItem {}\n"
_SINGLETON = "pragma Singleton\nimport QtQuick\n\nQtObject {}\n"


def test_a_view_written_with_a_leading_dot_slash_is_one_view_not_two():
    # './About.qml' and 'About.qml' are one file, spelled one way.
    source = maingen.render_client_main(
        {"name": "shop", "routes": [{"path": "/about", "view": "./About.qml"}]}, uri="Shop")
    assert "qrc:/qt/qml/Shop/About.qml" in source
    assert "/./" not in source

    cmake = _client_cmake([{"path": "/about", "view": "./About.qml"},
                           {"path": "/a", "view": "About"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS About.qml)") == 1


def test_a_view_in_a_subdirectory_keeps_its_subdirectory():
    # 'views/Home.qml' is aliased at the same relative path.
    source = maingen.render_client_main(
        {"name": "shop", "routes": [{"path": "/", "view": "views/Home.qml"}]}, uri="Shop")
    assert "qrc:/qt/qml/Shop/views/Home.qml" in source

    cmake = _client_cmake([{"path": "/", "view": "views/Home.qml"}])
    assert '"${SYNQT_GENERATED}/client/app/views/Home.qml"' in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS views/Home.qml)" in cmake


def test_a_route_with_no_view_is_refused_at_generation():
    # A route without a view does not default to Main.qml, the window. The generator refuses
    # it, since `synqt build` does not run the check.
    with pytest.raises(appmodel.AppGenError) as raised:
        maingen.render_client_main({"name": "shop", "routes": [{"path": "/admin"}]},
                                  uri="Shop")
    assert "/admin" in str(raised.value)
    assert "declares no view" in str(raised.value)

    with pytest.raises(appmodel.AppGenError):
        _client_cmake([{"path": "/admin", "view": ""}])


def test_a_view_reaching_outside_the_client_directory_is_refused_at_generation():
    # The escape rule holds in the generator too.
    for view in ("../web/A.qml", "..\\web\\A.qml", "/etc/A.qml", "C:/x/B.qml",
                 "C:\\x\\B.qml"):
        with pytest.raises(appmodel.AppGenError) as raised:
            maingen.render_client_main({"name": "shop",
                                       "routes": [{"path": "/a", "view": view}]}, uri="Shop")
        assert "absolute or parent path" in str(raised.value), view
        assert "/a" in str(raised.value), view
        with pytest.raises(appmodel.AppGenError):
            _client_cmake([{"path": "/a", "view": view}])


def test_the_escape_predicate_takes_the_views_that_are_really_paths():
    # 'a:b.qml' is a view, not a drive path.
    for accepted in ("Home.qml", "./Home.qml", "views/Home.qml", "Home", "a:b.qml",
                     "views/a:b.qml"):
        assert not appmodel.view_escapes_client_directory(accepted), accepted
    for rejected in ("../a.qml", "..\\a.qml", "/a.qml", "C:/a.qml", "C:\\a.qml",
                     "views/../../a.qml"):
        assert appmodel.view_escapes_client_directory(rejected), rejected


def test_two_qml_files_with_one_base_name_are_refused():
    # Two files that register one type name are refused.
    with pytest.raises(appmodel.AppGenError) as raised:
        _client_project({"Main.qml": _ITEM, "pages/Header.qml": _ITEM,
                         "widgets/Header.qml": _ITEM})
    message = str(raised.value)
    assert "pages/Header.qml" in message
    assert "widgets/Header.qml" in message
    assert "Header" in message


def test_a_route_view_that_is_also_on_disk_is_not_a_collision():
    # A route view that is also a swept file is listed once.
    cmake = _client_project({"Main.qml": _ITEM, "views/Home.qml": _ITEM},
                            routes=[{"path": "/", "view": "views/Home"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS views/Home.qml)") == 1


def test_a_hidden_qml_file_is_never_swept_in():
    # Dot-prefixed files are skipped (client/app/.Scratch.qml).
    cmake = _client_project({"Main.qml": _ITEM, ".Scratch.qml": _ITEM,
                            "parts/.Old.qml": _ITEM})
    assert "Scratch" not in cmake
    assert "Old.qml" not in cmake


def test_a_views_helper_components_are_compiled_in_too():
    # A sibling Card.qml is in the module.
    cmake = _client_project({"Main.qml": _ITEM, "Home.qml": _ITEM, "Card.qml": _ITEM,
                             "parts/Badge.qml": _ITEM},
                            routes=[{"path": "/", "view": "Home.qml"}])
    assert "PROPERTIES QT_RESOURCE_ALIAS Card.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS parts/Badge.qml)" in cmake
    assert '"${SYNQT_GENERATED}/client/app/parts/Badge.qml"' in cmake


def test_a_singleton_is_marked_as_one():
    # Theme.qml is marked QT_QML_SINGLETON_TYPE.
    cmake = _client_project({"Main.qml": _ITEM, "Theme.qml": _SINGLETON})
    assert "PROPERTIES QT_QML_SINGLETON_TYPE TRUE QT_RESOURCE_ALIAS Theme.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS Main.qml)" in cmake


def test_build_output_under_the_client_is_never_swept_in():
    # Build output under the entity is not compiled in.
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
    # Rendering from a config alone gives the route views only.
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"}])
    assert cmake.count("QT_RESOURCE_ALIAS") == 2


def test_every_route_view_is_in_the_clients_qml_module():
    # Every view is inside the QML module.
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"},
                           {"path": "/cart", "view": "Cart.qml"}])
    assert '"${SYNQT_GENERATED}/client/app/Home.qml"' in cmake
    assert '"${SYNQT_GENERATED}/client/app/Cart.qml"' in cmake
    # Each absolute path gets its module-root alias.
    assert "PROPERTIES QT_RESOURCE_ALIAS Home.qml)" in cmake
    assert "PROPERTIES QT_RESOURCE_ALIAS Cart.qml)" in cmake


def test_a_view_named_without_its_extension_is_listed_as_a_file():
    cmake = _client_cmake([{"path": "/", "view": "Home"}])
    assert '"${SYNQT_GENERATED}/client/app/Home.qml"' in cmake


def test_a_view_is_listed_once_however_many_routes_name_it():
    cmake = _client_cmake([{"path": "/", "view": "Home.qml"},
                           {"path": "/home", "view": "Home"}])
    assert cmake.count('"${SYNQT_GENERATED}/client/app/Home.qml"') == 2  # file + alias
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Home.qml)") == 1


def test_main_is_never_listed_twice():
    cmake = _client_cmake([{"path": "/", "view": "Main.qml"}])
    assert cmake.count("PROPERTIES QT_RESOURCE_ALIAS Main.qml)") == 1


def test_a_project_with_no_routes_compiles_an_empty_table():
    # No manufactured "/" -> Main.qml route.
    source = maingen.render_client_main({"name": "shop"}, uri="Shop")
    assert "config.routes = {};" in source
    cmake = _client_cmake([])
    assert cmake.count("QT_RESOURCE_ALIAS") == 1


def test_router_base_defaults_to_root_and_is_configurable():
    plain = maingen.render_client_main({"name": "shop", "routes": []}, uri="Shop")
    assert 'config.routerBase = QStringLiteral("/")' in plain

    based = maingen.render_client_main(
        {"name": "shop", "routes": [], "router": {"base": "/shop"}}, uri="Shop")
    assert 'config.routerBase = QStringLiteral("/shop")' in based


def test_router_fallback_defaults_to_root_and_is_configurable():
    plain = maingen.render_client_main({"name": "shop", "routes": []}, uri="Shop")
    assert 'config.routerFallback = QStringLiteral("/")' in plain

    redirected = maingen.render_client_main(
        {"name": "shop", "routes": [], "router": {"fallback": "/home"}}, uri="Shop")
    assert 'config.routerFallback = QStringLiteral("/home")' in redirected


def test_framework_root_honors_a_valid_synqt_root(tmp_path, monkeypatch):
    # SYNQT_ROOT names the framework for an installed CLI.
    (tmp_path / "src").mkdir()
    (tmp_path / "cmake").mkdir()
    monkeypatch.setenv("SYNQT_ROOT", str(tmp_path))
    assert appmodel.framework_root() == tmp_path.resolve()


def test_framework_root_rejects_a_root_without_sources(tmp_path, monkeypatch):
    # A wrong root fails here with a clear message.
    monkeypatch.setenv("SYNQT_ROOT", str(tmp_path))
    with pytest.raises(appmodel.AppGenError):
        appmodel.framework_root()


# The two edge routes the client's sign-in actions reach


def test_the_client_is_given_the_login_and_logout_routes():
    # The login and logout routes reach the client config.
    config = {
        "name": "shop",
        "identity": {"providers": [{"name": "github"}]},
    }
    source = maingen.render_client_main(config, uri="Shop")
    assert 'config.loginRoute = QStringLiteral("/auth/login");' in source
    assert 'config.logoutRoute = QStringLiteral("/auth/logout");' in source


def test_the_client_follows_the_routes_the_project_declared():
    # From the same block the edge is generated from.
    config = {
        "name": "shop",
        "identity": {"providers": [{"name": "github"}],
                     "login": "/enter", "logout": "/leave"},
    }
    source = maingen.render_client_main(config, uri="Shop")
    assert 'config.loginRoute = QStringLiteral("/enter");' in source
    assert 'config.logoutRoute = QStringLiteral("/leave");' in source


def test_a_project_with_no_identity_gets_no_routes():
    # Without identity both stay empty and calling either warns.
    source = maingen.render_client_main({"name": "shop"}, uri="Shop")
    assert "loginRoute" not in source
    assert "logoutRoute" not in source


def test_routes_for_prefers_the_entity_own_table():
    config = {"routes": [{"path": "/", "view": "Global.qml"}],
              "entities": [{"name": "app", "type": "client",
                            "routes": [{"path": "/", "view": "Own.qml"}]}]}
    entity = config["entities"][0]
    assert appmodel.routes_for(config, entity) == [{"path": "/", "view": "Own.qml"}]


def test_routes_for_falls_back_to_the_top_level_shorthand():
    config = {"routes": [{"path": "/", "view": "Global.qml"}],
              "entities": [{"name": "app", "type": "client"}]}
    entity = config["entities"][0]
    assert appmodel.routes_for(config, entity) == [{"path": "/", "view": "Global.qml"}]


def test_routes_for_without_an_entity_reads_the_top_level_block():
    config = {"routes": [{"path": "/", "view": "Global.qml"}]}
    assert appmodel.routes_for(config) == [{"path": "/", "view": "Global.qml"}]


def test_routes_for_drops_non_mapping_entries():
    config = {"routes": ["not-a-route", {"path": "/", "view": "Home.qml"}]}
    assert appmodel.routes_for(config) == [{"path": "/", "view": "Home.qml"}]


def test_routes_for_treats_an_empty_own_list_as_declared():
    # `routes: []` on a client does not inherit the shorthand.
    config = {"routes": [{"path": "/", "view": "Global.qml"}],
              "entities": [{"name": "app", "type": "client", "routes": []}]}
    assert appmodel.routes_for(config, config["entities"][0]) == []


def test_route_views_reads_the_entity_own_table():
    config = {"routes": [{"path": "/", "view": "Global.qml"}],
              "entities": [{"name": "app", "type": "client",
                            "routes": [{"path": "/", "view": "Own.qml"},
                                       {"path": "/b", "view": "Other.qml"}]}]}
    assert appmodel.route_views(config, config["entities"][0]) == ["Own.qml", "Other.qml"]


def test_route_views_without_an_entity_is_unchanged():
    config = {"routes": [{"path": "/", "view": "Home.qml"}]}
    assert appmodel.route_views(config) == ["Home.qml"]


@pytest.mark.parametrize("name", ['db"x', "../outside", "has space", "9lives", ""])
def test_an_entity_name_the_generated_code_cannot_carry_is_refused(name):
    # Generation runs before any check, and an entity name becomes a C++ string, a CMake
    # target and a directory under generated/. A quote or a `..` must stop here.
    from synqt import appgen
    with tempfile.TemporaryDirectory() as tmp:
        config = {"project": {"name": "p"},
                  "entities": [{"name": "app", "type": "client"},
                               {"name": "edge", "type": "web_edge"},
                               {"name": name, "type": "relational"}]}
        with pytest.raises(appmodel.AppGenError, match="entity name"):
            appgen.generate(tmp, config)
        assert not (Path(tmp).parent / "outside").exists()


def test_a_valid_entity_name_generates():
    from synqt import appgen
    with tempfile.TemporaryDirectory() as tmp:
        config = {"project": {"name": "p"},
                  "entities": [{"name": "app", "type": "client"},
                               {"name": "edge", "type": "web_edge"},
                               {"name": "store-2", "type": "relational"}]}
        written = appgen.generate(tmp, config)
        assert any("store-2" in path for path in written)
