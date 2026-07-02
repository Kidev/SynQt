# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The monitoring fan-in: one link, derived rather than written, and never to a client."""

from pathlib import Path

import pytest

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
    # A client never consumes the ingest point: its entity name would be visitor-controlled.
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


# The link is synthesized, so its errors are reported here.


def _messages(config):
    entities = {entity["name"]: entity for entity in config["entities"]}
    return check._monitor_entity_messages(config, entities)


def test_a_monitor_entity_that_does_not_exist_is_refused():
    config = _config()
    config["monitoring"] = {"entity": "nope"}
    findings = _messages(config)
    assert any(message.startswith("error:") and "not a declared entity" in message
               for message in findings), findings


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


def test_a_project_with_no_monitor_at_all_says_nothing():
    config = _config(monitoring=False)
    config["entities"] = [entity for entity in config["entities"]
                          if entity["name"] != "ops"]
    assert _messages(config) == []


# A monitor that `monitoring.entity` does not name receives nothing.


def test_a_monitor_nobody_reports_to_is_reported():
    findings = _messages(_config(monitoring=False))
    assert any(message.startswith("warn:") and "'ops'" in message
               and "no monitoring.entity" in message for message in findings), findings


def test_a_second_monitor_beside_the_wired_one_is_reported():
    config = _config(extra=[{"name": "spare", "type": "monitor"}])
    findings = _messages(config)
    assert any(message.startswith("warn:") and "'spare'" in message
               and "names 'ops' instead" in message for message in findings), findings
    assert not any("'ops' has 'type: monitor'" in message for message in findings), findings


# The resolved topology. What each entity's binary is handed.


def _topology(name, config=None):
    config = appmodel.with_monitoring_connect_points(config or _config())
    config.setdefault("project", {"name": "x"})
    entity = next(e for e in config["entities"] if e["name"] == name)
    endpoints = topologywriter.resolve_endpoints(config, "x")
    return topologywriter.entity_topology(config, entity, Path("/p"), endpoints)


def test_a_reporting_entity_is_told_where_it_may_spool():
    # The spool is inside the project.
    spool = _topology("web")["monitoring"]["spool_dir"]
    # Forward slashes on every host (topologywriter._path). The root is not asserted:
    # Path("/p") gains a drive on Windows.
    assert spool == f"{Path('/p').resolve().as_posix()}/build/web/state"



# What the design editor is handed, so it can draw one with no SynQt behind the page.


def test_the_editors_monitor_asset_is_the_scaffolders_own_answer():
    """The committed editor asset matches monitorscaffold's output byte for byte."""
    import importlib.util

    repo = Path(__file__).resolve().parents[3]
    writer_path = repo / "tools" / "gen-design-assets.py"
    spec = importlib.util.spec_from_file_location("gen_design_assets", writer_path)
    writer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(writer)

    for relative, (name, answer) in writer.ASSETS.items():
        committed = repo / relative
        assert committed.exists(), f"{relative} is missing; run `python tools/gen-design-assets.py`"
        assert committed.read_text(encoding="utf-8") == writer.rendered(name, answer()), (
            f"{relative} is out of date; run `python tools/gen-design-assets.py`")


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
    # GPLv3, like the edge.
    assert licenses.effective_license(modules) == "GPL-3.0-only"


# The console's point, the scaffold, and the two rules a monitor changes.


def _console_config():
    config = _config()
    config["entities"].append({"name": "console", "type": "client", "console": True,
                               "edge": "ops"})
    return config


def test_the_console_gets_its_own_point_gated_on_operator():
    points = {point["name"]: point for point
              in appmodel.monitoring_connect_points(_console_config())}
    assert set(points) == {"ingest", "console"}
    assert points["console"]["scope"] == appmodel.MONITOR_SCOPE
    assert points["console"]["consumers"] == ["console"]


def test_an_ordinary_client_is_not_offered_the_console():
    # `console: true` marks a console client.
    points = {point["name"]: point for point
              in appmodel.monitoring_connect_points(_console_config())}
    assert "app" not in points["console"]["consumers"]


def test_a_project_with_no_console_client_gets_no_console_point():
    names = {point["name"] for point in appmodel.monitoring_connect_points(_config())}
    assert names == {"ingest"}


def test_a_browser_may_reach_a_monitor():
    # A monitor is browser-facing; is_edge still means the application edge.
    assert appmodel.serves_browser({"name": "ops", "type": "monitor"})
    assert appmodel.serves_browser({"name": "web", "type": "web_edge"})
    assert not appmodel.serves_browser({"name": "db", "type": "relational"})
    assert not appmodel.is_edge({"name": "ops", "type": "monitor"})


def test_a_second_client_still_has_no_session_field_to_forge():
    # `forwards_session` considers every client, not only the first.
    config = _console_config()
    console_point = {"owner": "ops", "consumers": ["console"]}
    assert not appmodel.forwards_session(config, console_point)
    app_point = {"owner": "web", "consumers": ["app"]}
    assert not appmodel.forwards_session(config, app_point)
    # And a service consumer still carries it.
    assert appmodel.forwards_session(config, {"owner": "ops", "consumers": ["web"]})


def test_scaffolding_a_monitor_writes_all_four_things_it_is_made_of(tmp_path):
    from synqt import addentity

    (tmp_path / "synqt.yaml").write_text(
        "project:\n  name: demo\n\nentities:\n  - name: app\n    type: client\n"
        "  - name: web\n    type: web_edge\n")
    addentity.scaffold(tmp_path, "ops", "monitor")

    import yaml
    config = yaml.safe_load((tmp_path / "synqt.yaml").read_text())
    names = {entity["name"]: entity for entity in config["entities"]}
    assert "ops" in names and names["ops"]["type"] == "monitor"
    assert "ops-console" in names and names["ops-console"]["console"] is True
    # The line that makes every service report. Without it the other three do nothing.
    assert appmodel.monitor_entity(config) == "ops"
    # The gate. An anonymous visitor is handed a different bundle, not a 403 on the console.
    assert names["ops"]["bundles"]["anonymous"] == "signin/"
    assert names["ops"]["bundles"]["operator"] == "ops-console"
    assert (tmp_path / "monitor/ops/signin/index.html").exists()
    assert (tmp_path / "client/ops-console/Main.qml").exists()


def test_the_console_listens_on_loopback_by_default(tmp_path):
    from synqt import addentity
    import yaml

    (tmp_path / "synqt.yaml").write_text("entities: []\n")
    addentity.scaffold(tmp_path, "ops", "monitor")
    config = yaml.safe_load((tmp_path / "synqt.yaml").read_text())
    monitor = next(e for e in config["entities"] if e["name"] == "ops")
    # The console binds loopback by default.
    assert monitor["public"]["host"] == "127.0.0.1"


def test_scaffolding_twice_is_refused_rather_than_doubling_the_console(tmp_path):
    from synqt import addentity

    (tmp_path / "synqt.yaml").write_text("entities: []\n")
    addentity.scaffold(tmp_path, "ops", "monitor")
    with pytest.raises(addentity.AddEntityError):
        addentity.scaffold(tmp_path, "ops", "monitor")


def _findings(config):
    entities = {entity["name"]: entity for entity in config["entities"]}
    return check._monitor_entity_messages(config, entities)


def test_how_much_each_category_records_is_a_deployment_setting():
    config = _config()
    config["monitoring"]["levels"] = {"call": "debug", "data": "off"}
    assert _findings(config) == []
    # Levels reach the entity through the resolved topology, changed without a rebuild.
    assert appmodel.trace_levels(config) == {"call": "debug", "data": "off"}


def test_a_category_nobody_spelled_right_is_refused():
    config = _config()
    config["monitoring"]["levels"] = {"calls": "debug"}
    findings = _findings(config)
    # It fails as an empty console otherwise, which reads as a category with nothing in it.
    assert any("unknown category 'calls'" in finding for finding in findings)


def test_a_level_nobody_spelled_right_is_refused():
    config = _config()
    config["monitoring"]["levels"] = {"call": "verbose"}
    findings = _findings(config)
    assert any("unknown level 'verbose'" in finding for finding in findings)


def test_the_levels_reach_a_reporting_entity_and_not_the_monitor(tmp_path):
    config = _config()
    config["monitoring"]["levels"] = {"call": "debug"}
    topologywriter.write(tmp_path, config)
    import json

    edge = json.loads((tmp_path / "build" / "web" / "topology.json").read_text())
    assert edge["monitoring"]["levels"] == {"call": "debug"}
    # The monitor reports to nobody, so it grows neither a spool nor a level table.
    monitor = json.loads((tmp_path / "build" / "ops" / "topology.json").read_text())
    assert "monitoring" not in monitor


def test_a_monitor_on_a_public_interface_has_to_say_so():
    config = _config()
    monitor = config["entities"][0]
    monitor["public"] = {"host": "0.0.0.0", "port": 8444}
    findings = _findings(config)
    assert len(findings) == 1
    # A public bind must be acknowledged; not refused, for deployments behind an
    # authenticating proxy.
    assert "reachable from off this machine" in findings[0]

    config["monitoring"]["public"] = "acknowledged"
    assert _findings(config) == []


def test_the_application_client_cannot_consume_what_the_monitor_owns():
    config = _config()
    config["connect_points"] = [{"owner": "ops", "consumers": ["app"],
                                 "export": "prop string headline\n"}]
    findings = _findings(config)
    assert len(findings) == 1
    # The application client may not consume a monitor point.
    assert "only client that may read one is its console" in findings[0]


def test_a_monitor_is_refused_as_a_consumer():
    # A monitor may not consume a declared point.
    config = _config()
    config["connect_points"] = [{"owner": "web", "consumers": ["db", "ops"],
                                 "export": "prop string headline\n"}]
    entities = {entity["name"]: entity for entity in config["entities"]}
    findings = check._monitor_as_consumer_messages(config, entities)
    assert len(findings) == 1
    assert findings[0].startswith("error:")
    assert "'ops'" in findings[0]
    # Only the monitor is refused, not the other consumers.
    assert "'db'" not in findings[0]


def test_a_point_the_monitor_owns_is_not_refused_by_that_rule():
    # Owning a point is checked by _monitor_consumer_messages, not refused here.
    config = _config(extra=[{"name": "ops-console", "type": "client", "console": True,
                             "edge": "ops"}])
    config["connect_points"] = [{"owner": "ops", "consumers": ["ops-console"],
                                 "export": "prop string headline\n"}]
    entities = {entity["name"]: entity for entity in config["entities"]}
    assert check._monitor_as_consumer_messages(config, entities) == []


def test_the_console_client_may():
    config = _config(extra=[{"name": "ops-console", "type": "client", "console": True,
                             "edge": "ops"}])
    config["connect_points"] = [{"owner": "ops", "consumers": ["ops-console"],
                                 "export": "prop string headline\n"}]
    assert _findings(config) == []


def test_two_browser_facing_entities_cannot_share_a_port():
    entities = {
        "web": {"name": "web", "type": "web_edge",
                "public": {"host": "127.0.0.1", "port": 8443}},
        "ops": {"name": "ops", "type": "monitor",
                "public": {"host": "127.0.0.1", "port": 8443}},
    }
    findings = check._public_port_messages(entities)
    # Two browser-facing entities on one port.
    assert len(findings) == 1
    assert "both serve browsers on 127.0.0.1:8443" in findings[0]

    entities["ops"]["public"]["port"] = 8444
    assert check._public_port_messages(entities) == []


def test_a_port_nobody_wrote_down_is_still_a_port_both_of_them_bind():
    """An unset `public.port` counts as the default, so an edge and a monitor that both omit it
    collide.
    """
    entities = {
        "web": {"name": "web", "type": "web_edge"},
        "ops": {"name": "ops", "type": "monitor", "public": {"host": "127.0.0.1"}},
    }
    findings = check._public_port_messages(entities)
    assert len(findings) == 1
    assert f"both serve browsers on 127.0.0.1:{appmodel.DEFAULT_PUBLIC_PORT}" in findings[0]


def test_the_scaffolder_steps_past_the_default_the_edge_never_wrote_down(tmp_path):
    """And the scaffolder had the same hole, from the same question asked the same way."""
    from synqt import addentity, monitorscaffold
    import yaml

    (tmp_path / "synqt.yaml").write_text(
        "entities:\n"
        "  - name: web\n"
        "    type: web_edge\n")
    addentity.scaffold(tmp_path, "ops", "monitor")
    config = yaml.safe_load((tmp_path / "synqt.yaml").read_text())
    monitor = next(e for e in config["entities"] if e["name"] == "ops")
    assert monitor["public"]["port"] == appmodel.DEFAULT_PUBLIC_PORT + 1
    assert check._public_port_messages(
        {e["name"]: e for e in config["entities"]}) == []


def test_the_scaffolder_steps_past_a_port_the_edge_already_has(tmp_path):
    from synqt import addentity, monitorscaffold
    import yaml

    (tmp_path / "synqt.yaml").write_text(
        "entities:\n"
        "  - name: web\n"
        "    type: web_edge\n"
        "    public:\n"
        "      host: 127.0.0.1\n"
        "      port: 8443\n")
    addentity.scaffold(tmp_path, "ops", "monitor")
    config = yaml.safe_load((tmp_path / "synqt.yaml").read_text())
    monitor = next(e for e in config["entities"] if e["name"] == "ops")
    # Written free rather than written colliding for `synqt check` to report afterwards.
    assert monitor["public"]["port"] == 8444
    assert monitorscaffold.free_port(config) == 8445


def test_the_monitor_is_told_where_the_bundles_are_and_not_what_they_are_called():
    from synqt import maingen

    config = {
        "project": {"name": "demo"},
        "monitoring": {"entity": "ops"},
        "entities": [
            {"name": "ops", "type": "monitor",
             "bundles": {"anonymous": "signin/", "operator": "ops-console"}},
            {"name": "app", "type": "client"},
            {"name": "ops-console", "type": "client", "console": True, "edge": "ops"},
        ],
    }
    source = maingen.render_monitor_main(config, config["entities"][0])
    # `bundles:` resolves to where the build put each bundle.
    assert '"anonymous=monitor/ops/signin"' in source
    assert '"operator=build/client-ops-console"' in source
    # As option defaults, so a deployment can pass --bundle.
    assert "bundleWithDefaults.setDefaultValues" in source
    assert 'parser.values(bundleWithDefaults)' in source


def test_a_monitor_behind_a_proxy_is_told_which_peer_is_not_an_operator():
    from synqt import maingen

    # The console sign-in is rate-limited per address, so the monitor main carries
    # `public.trusted_proxies`.
    config = {
        "project": {"name": "demo"},
        "monitoring": {"entity": "ops"},
        "entities": [{"name": "ops", "type": "monitor",
                      "public": {"trusted_proxies": ["10.0.0.1", "10.0.0.0/24"]}}],
    }
    source = maingen.render_monitor_main(config, config["entities"][0])
    assert ('config.trustedProxies = {QStringLiteral("10.0.0.1"), '
            'QStringLiteral("10.0.0.0/24")};') in source

    config["entities"][0].pop("public")
    assert "trustedProxies" not in maingen.render_monitor_main(config,
                                                               config["entities"][0])


def test_synqt_dev_launches_a_monitor_with_its_own_port_and_its_bundles():
    from pathlib import Path
    from synqt import run

    config = {
        "project": {"name": "demo"},
        "monitoring": {"entity": "ops"},
        "entities": [
            {"name": "ops", "type": "monitor",
             "public": {"host": "127.0.0.1", "port": 8444},
             "bundles": {"anonymous": "signin/", "operator": "ops-console"}},
            {"name": "app", "type": "client"},
            {"name": "ops-console", "type": "client", "console": True, "edge": "ops"},
        ],
    }
    command = run.dev_command(Path("/p"), config["entities"][0], config, 8080)
    # Its own port from `public:`, not the edge's 8080.
    assert "--port" in command and command[command.index("--port") + 1] == "8444"
    assert f"anonymous={Path('/p') / 'monitor' / 'ops' / 'signin'}" in command
    assert f"operator={Path('/p') / 'build' / 'client-ops-console'}" in command
    # Both halves. It hosts a mesh point as well as serving a console.
    assert "--topology" in command and "--qml-dir" in command
