# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The generated client carries a component URL per route, and the router base."""

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
