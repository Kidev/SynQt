# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The `bundles:` block: which scope is served which bundle, and where it is built."""

from synqt import appmodel


def _config(bundles=None, clients=("app",)):
    edge = {"name": "web", "type": "web_edge"}
    if bundles is not None:
        edge["bundles"] = bundles
    entities = [edge] + [{"name": name, "type": "client"} for name in clients]
    return {"entities": entities, "scopes": {"order": ["anonymous", "user"]}}, edge


def test_no_block_serves_the_one_client_to_the_default_scope():
    config, edge = _config()
    assert appmodel.bundles_for(config, edge) == {
        "anonymous": (appmodel.BUNDLE_CLIENT, "app")}


def test_a_bare_name_is_a_client_entity():
    config, edge = _config({"user": "app"})
    assert appmodel.bundles_for(config, edge) == {"user": (appmodel.BUNDLE_CLIENT, "app")}


def test_a_value_with_a_slash_is_a_static_directory():
    config, edge = _config({"anonymous": "landing/"})
    assert appmodel.bundles_for(config, edge) == {
        "anonymous": (appmodel.BUNDLE_STATIC, "landing/")}


def test_both_kinds_together():
    config, edge = _config({"anonymous": "landing/", "user": "app"})
    assert appmodel.bundles_for(config, edge) == {
        "anonymous": (appmodel.BUNDLE_STATIC, "landing/"),
        "user": (appmodel.BUNDLE_CLIENT, "app")}


def test_a_single_client_project_keeps_the_historic_output_path():
    config, _ = _config()
    client = config["entities"][1]
    assert appmodel.bundle_output_dir(config, client) == "build/client"


def test_a_multi_client_project_gets_a_directory_per_client():
    config, _ = _config(clients=("app", "gate"))
    app = config["entities"][1]
    gate = config["entities"][2]
    assert appmodel.bundle_output_dir(config, app) == "build/client-app"
    assert appmodel.bundle_output_dir(config, gate) == "build/client-gate"
