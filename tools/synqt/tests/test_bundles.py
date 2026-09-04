# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The `bundles:` block: which scope is served which bundle, and where it is built."""

import re

from synqt import appmodel, check


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


def test_one_client_keeps_the_project_qml_uri():
    config, _ = _config()
    config["project"] = {"name": "shop"}
    assert appmodel.qml_uri_for(config, config["entities"][1]) == "Shop"


def test_two_clients_get_distinct_qml_uris():
    config, _ = _config(clients=("app", "gate"))
    config["project"] = {"name": "shop"}
    app = appmodel.qml_uri_for(config, config["entities"][1])
    gate = appmodel.qml_uri_for(config, config["entities"][2])
    # Distinct URIs, or both modules would claim qrc:/qt/qml/Shop/Main.qml.
    assert app != gate
    assert app == "ShopApp"
    assert gate == "ShopGate"


def test_a_hyphenated_client_name_still_makes_a_legal_qml_module_uri():
    """A hyphenated client name (`<name>-console`) still gives a legal QML module URI, folded
    through `qml_uri` like the project name.
    """
    config, _ = _config(clients=("app", "ops-console"))
    config["project"] = {"name": "watched"}
    console = appmodel.qml_uri_for(config, config["entities"][2])
    assert console == "WatchedOpsConsole"
    for uri in (appmodel.qml_uri_for(config, config["entities"][1]), console):
        assert re.fullmatch(r"[A-Za-z_][0-9A-Za-z_]*", uri), uri


def test_two_client_names_that_fold_to_one_uri_are_refused():
    """Two client names that fold to one URI (`admin-ui`, `admin_ui`) are refused."""
    config, _ = _config(clients=("admin-ui", "admin_ui"))
    config["project"] = {"name": "shop"}
    ok, messages = check.validate(config)
    assert not ok
    assert any("admin-ui" in m and "admin_ui" in m and "ShopAdminUi" in m
               for m in messages), messages


def test_an_edge_started_with_no_arguments_serves_each_scope_its_own_bundle():
    """`synqt serve`, a container and a supervisor fed from the start plan run the edge with
    no `--bundle`. The defaults it was generated with must keep the anonymous scope on its
    own files, or every visitor is sent the client the block keeps from them.
    """
    from synqt import maingen
    config, edge = _config({"anonymous": "landing/", "user": "app"})
    main = maingen.render_edge_main(config, edge)
    defaults = re.search(r"bundleOption\.setDefaultValues\(\{(.*?)\}\);", main, re.S)
    assert defaults, main
    assert re.findall(r'QStringLiteral\("([^"]*)"\)', defaults.group(1)) == [
        "anonymous=web/web/landing", "user=build/client"]
    assert 'QStringLiteral("build/client")}' not in main


def test_an_edge_with_no_block_defaults_to_its_one_clients_bundle():
    from synqt import maingen
    config, edge = _config(clients=("app", "gate"))
    main = maingen.render_edge_main(config, edge)
    assert 'bundleOption.setDefaultValues({QStringLiteral("build/client-app")});' in main


def test_dev_reloads_every_clients_bundle(tmp_path):
    from synqt import run as runmod
    config, _ = _config({"anonymous": "app", "user": "gate"}, clients=("app", "gate"))
    for name in ("app", "gate"):
        (tmp_path / "build" / f"client-{name}").mkdir(parents=True)
    runmod._write_dev_reload_harnesses(tmp_path, config)
    for name in ("app", "gate"):
        assert (tmp_path / "build" / f"client-{name}" / "synqt-reload.txt").is_file(), name
