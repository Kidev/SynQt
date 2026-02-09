# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The monitoring fan-in: one link, derived rather than written, and never to a client."""

from pathlib import Path

from synqt import appmodel, check, cmakegen, licenses, topologywriter


def _config(monitoring=True, extra=()):
    entities = [{"name": "ops", "type": "monitor"},
                {"name": "web", "type": "web_edge"},
                {"name": "db", "type": "relational"},
                {"name": "app", "type": "client"}]
    entities += list(extra)
    config = {"entities": entities}
    if monitoring:
        config["monitoring"] = {"entity": "ops"}
    return config


def test_the_monitor_owns_one_point_every_service_consumes():
    points = appmodel.monitoring_connect_points(_config())
    assert len(points) == 1
    point = points[0]
    assert point["owner"] == "ops"
    assert point["contract"] == "Ingest"
    assert point["framework"] is True


def test_a_client_never_consumes_it():
    # A browser cannot reach the mesh, and a client reporting events as an entity would be
    # putting a value a visitor controls where an authenticated entity name belongs.
    point = appmodel.monitoring_connect_points(_config())[0]
    assert set(point["consumers"]) == {"web", "db"}
    assert "app" not in point["consumers"]


def test_the_monitor_does_not_consume_its_own_point():
    point = appmodel.monitoring_connect_points(_config())[0]
    assert "ops" not in point["consumers"]


def test_no_monitoring_key_means_no_link():
    assert appmodel.monitoring_connect_points(_config(monitoring=False)) == []


def test_the_server_file_is_generated_and_not_the_authors():
    point = appmodel.monitoring_connect_points(_config())[0]
    assert point["server"].startswith(appmodel.GENERATED_DIR + "/")
    assert point["server"].endswith("/Ingest.qml")


def test_a_declared_point_of_the_same_name_wins():
    # The project meant it. `synqt check` is what reports the collision.
    config = _config()
    config["connect_points"] = [{"owner": "ingest", "consumers": ["web"], "export": "prop int n"}]
    assert appmodel.monitoring_connect_points(config) == []


def test_the_expansion_is_idempotent():
    once = appmodel.with_monitoring_connect_points(_config())
    twice = appmodel.with_monitoring_connect_points(once)
    assert len(appmodel.connect_points(twice)) == len(appmodel.connect_points(once))


def test_the_expansion_does_not_mutate_its_input():
    config = _config()
    appmodel.with_monitoring_connect_points(config)
    assert appmodel.connect_points(config) == []


def test_monitor_is_a_type_the_config_accepts():
    ok, messages = check.validate(_config())
    assert not any("unknown type 'monitor'" in message for message in messages), messages


# What the project cannot see for itself. The link is synthesized, so everything that could
# be wrong with it has to be said here or it is not said anywhere.


def _messages(config):
    entities = {entity["name"]: entity for entity in config["entities"]}
    return check._monitor_entity_messages(config, entities)


def test_a_monitor_entity_that_does_not_exist_is_refused():
    config = _config()
    config["monitoring"] = {"entity": "nope"}
    findings = _messages(config)
    assert len(findings) == 1 and findings[0].startswith("error:")
    assert "not a declared entity" in findings[0]


def test_pointing_it_at_an_entity_of_another_type_is_refused():
    config = _config()
    config["monitoring"] = {"entity": "web"}
    findings = _messages(config)
    assert any("type: monitor" in message for message in findings), findings


def test_a_declared_point_of_the_same_name_is_reported():
    # The expansion steps around the collision silently, so it is reported before it.
    config = _config()
    config["connect_points"] = [{"owner": "ingest", "consumers": ["web"], "export": "prop int n"}]
    findings = _messages(config)
    assert any("collides" in message for message in findings), findings


def test_an_unknown_key_is_refused_rather_than_ignored():
    config = _config()
    config["monitoring"] = {"entity": "ops", "levl": "info"}
    assert any("unknown key 'levl'" in message for message in _messages(config))


def test_monitoring_has_to_be_a_block():
    config = _config()
    config["monitoring"] = "ops"
    assert any("must be a block" in message for message in _messages(config))


def test_a_project_with_no_monitor_says_nothing():
    assert _messages(_config(monitoring=False)) == []


# The resolved topology: what each entity's binary is actually handed.


def _topology(name, config=None):
    config = appmodel.with_monitoring_connect_points(config or _config())
    config.setdefault("project", {"name": "x"})
    entity = next(e for e in config["entities"] if e["name"] == name)
    endpoints = topologywriter.resolve_endpoints(config, "x")
    return topologywriter.entity_topology(config, entity, Path("/p"), endpoints)


def test_a_reporting_entity_is_told_where_it_may_spool():
    # Inside the project. A spool is a copy of the record, and a copy of the record living
    # somewhere the project does not own is a copy nobody is watching.
    spool = _topology("web")["monitoring"]["spool_dir"]
    assert spool == "/p/build/web/state"


def test_the_monitor_itself_gets_no_spool():
    assert "monitoring" not in _topology("ops")


def test_a_client_gets_no_spool_and_no_link():
    topology = _topology("app")
    assert "monitoring" not in topology
    assert topology.get("connect_points", []) == []


def test_a_project_with_no_monitor_writes_no_spool():
    assert "monitoring" not in _topology("web", _config(monitoring=False))


# What a monitor entity is made of. Its library, and the license that follows from it.


def test_a_monitor_links_its_own_runtime_library():
    entity = {"name": "ops", "type": "monitor"}
    assert appmodel.service_libraries(_config(), entity) == ["SynQtMonitor"]


def test_the_generated_build_adds_the_monitor_library():
    config = {"project": {"name": "x"}, "monitoring": {"entity": "ops"},
              "entities": [{"name": "ops", "type": "monitor"},
                           {"name": "web", "type": "web_edge"}]}
    lines = cmakegen._runtime_library_cmake(config, config["entities"])
    assert any("src/monitor" in line for line in lines)
    linked = cmakegen._service_cmake(config, config["entities"][0])
    assert any("SynQtMonitor" in line for line in linked)


def test_the_monitor_reports_the_modules_it_actually_links():
    config = {"entities": [{"name": "ops", "type": "monitor"}]}
    modules = licenses.entity_modules(config["entities"][0], "native", config)
    # It serves the console over HTTP and keeps a history.
    assert "Qt HTTP Server" in modules
    assert "Qt Sql" in modules
    # Which makes it GPLv3, like the edge. That is a fact about an operations tool, not
    # about anything a project conveys to its visitors.
    assert licenses.effective_license(modules) == "GPL-3.0-only"
