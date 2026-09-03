# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The cold tier: the `export:` block, what it refuses, and what it generates."""

import pytest

from synqt import check, maingen


def _config(export=None):
    monitor = {"name": "ops", "type": "monitor",
               "public": {"host": "127.0.0.1", "port": 8443}}
    if export is not None:
        monitor["export"] = export
    return {
        "project": {"name": "demo"},
        "monitoring": {"entity": "ops"},
        "entities": [{"name": "web", "type": "web_edge"},
                     {"name": "app", "type": "client"},
                     monitor],
        "connect_points": [{"owner": "web", "consumers": ["app"]}],
    }


def _monitor_findings(export, release=False):
    config = _config(export)
    entities = {entity["name"]: entity for entity in config["entities"]}
    return check._monitor_entity_messages(config, entities, release)


def test_a_monitor_with_no_export_block_is_the_normal_case():
    # Exporting is opt-in; its absence is not a finding.
    assert _monitor_findings(None) == []


def test_a_collector_endpoint_is_accepted():
    assert _monitor_findings({"otlp": {"endpoint": "https://collector:4318"}}) == []


def test_an_otlp_block_with_no_endpoint_is_refused():
    findings = _monitor_findings({"otlp": {"timeout_ms": 1000}})
    assert len(findings) == 1
    # An exporter without a destination is refused.
    assert findings[0].startswith("error:")
    assert "names no endpoint" in findings[0]


def test_an_endpoint_that_is_not_an_http_url_is_refused():
    findings = _monitor_findings({"otlp": {"endpoint": "collector:4317"}})
    assert len(findings) == 1
    assert "is not an http(s) URL" in findings[0]


def test_plaintext_to_a_remote_collector_is_warned_about():
    findings = _monitor_findings({"otlp": {"endpoint": "http://collector.internal:4318"}})
    assert len(findings) == 1
    assert findings[0].startswith("warn:")
    assert "API key" in findings[0]


def test_plaintext_to_a_collector_on_this_machine_is_not():
    # It never leaves the machine, and this is the ordinary deployment.
    assert _monitor_findings({"otlp": {"endpoint": "http://127.0.0.1:4318"}}) == []
    assert _monitor_findings({"otlp": {"endpoint": "http://localhost:4318"}}) == []


def test_a_release_build_refuses_plaintext_to_a_remote_collector():
    # A plaintext remote collector is refused in a release build; the runtime would refuse
    # it.
    findings = _monitor_findings({"otlp": {"endpoint": "http://collector.internal:4318"}},
                                 release=True)
    assert len(findings) == 1
    assert findings[0].startswith("error:")


def test_a_release_build_still_allows_https_and_this_machine():
    assert _monitor_findings({"otlp": {"endpoint": "https://api.honeycomb.io"}},
                             release=True) == []
    assert _monitor_findings({"otlp": {"endpoint": "http://127.0.0.1:4318"}},
                             release=True) == []


def test_a_jsonl_file_with_no_cap_is_warned_about():
    findings = _monitor_findings({"jsonl": {"path": "build/ops/events.jsonl",
                                            "max_bytes": 0}})
    assert len(findings) == 1
    assert findings[0].startswith("warn:")
    assert "fills the disk" in findings[0]


def test_a_jsonl_block_that_keeps_nothing_is_refused():
    findings = _monitor_findings({"jsonl": {"path": "build/ops/events.jsonl", "keep": 0}})
    assert any("keeps 0 rotations" in finding for finding in findings)


def test_a_jsonl_bound_that_is_not_a_number_is_refused_rather_than_crashing():
    findings = _monitor_findings({"jsonl": {"path": "build/ops/events.jsonl",
                                            "max_bytes": "64MB", "keep": "five"}})
    assert any("whole numbers" in finding for finding in findings), findings


@pytest.mark.parametrize("block, key, value", [
    ("retention", "max_age_days", "14d"),
    ("retention", "max_bytes", -1),
    ("export.otlp", "max_in_flight", 0),
    ("export.otlp", "timeout_ms", True),
])
def test_a_monitor_number_out_of_shape_is_refused(block, key, value):
    """Each is written into the monitor's generated main, which would otherwise stop with a
    Python traceback or compile a value that means something else.
    """
    config = _config({"otlp": {"endpoint": "http://127.0.0.1:4318"}})
    monitor = next(entity for entity in config["entities"] if entity.get("type") == "monitor")
    settings = monitor
    for part in block.split("."):
        settings = settings.setdefault(part, {})
    settings[key] = value
    ok, messages = check.validate(config)
    assert not ok
    assert any(f"{block}.{key}" in message for message in messages), messages


def test_an_unknown_export_key_is_refused():
    findings = _monitor_findings({"otel": {"endpoint": "http://127.0.0.1:4318"}})
    assert any("unknown key 'otel'" in finding for finding in findings)


def test_a_monitor_that_exports_nothing_links_no_network_client():
    config = _config(None)
    source = maingen.render_monitor_main(config, config["entities"][2])
    # Without `export:` the exporter header is not included.
    assert "otlpexporter.h" not in source
    assert "jsonlexporter.h" not in source
    assert "addExporter" not in source


def test_the_collectors_api_key_is_read_from_the_environment_and_never_from_the_project():
    config = _config({"otlp": {"endpoint": "https://api.honeycomb.io",
                               "max_in_flight": 4, "timeout_ms": 2000}})
    source = maingen.render_monitor_main(config, config["entities"][2])
    assert 'otlpSettings.endpoint = QUrl{QStringLiteral("https://api.honeycomb.io")}' in source
    assert "otlpSettings.maxInFlight = 4;" in source
    assert "otlpSettings.timeoutMs = 2000;" in source
    # The credential comes from the entity environment; there is no key for it in
    # synqt.yaml.
    assert "OtlpExporter::headersFromEnvironment()" in source
    assert "service.addExporter(&otlpExporter);" in source


def test_the_jsonl_file_carries_its_bound_into_the_generated_main():
    config = _config({"jsonl": {"path": "build/ops/state/events.jsonl",
                                "max_bytes": 1048576, "keep": 3}})
    source = maingen.render_monitor_main(config, config["entities"][2])
    assert 'JsonlExporter jsonlExporter{QStringLiteral("build/ops/state/events.jsonl")' \
        in source
    assert "1048576LL," in source
    assert "3};" in source
    assert "service.addExporter(&jsonlExporter);" in source


def test_exporting_is_wired_after_the_store_and_not_instead_of_it():
    config = _config({"jsonl": {"path": "build/ops/state/events.jsonl"}})
    source = maingen.render_monitor_main(config, config["entities"][2])
    # SynQt's own history always records; an exporter is an additional destination.
    assert source.index("EventStore store{") < source.index("addExporter")
