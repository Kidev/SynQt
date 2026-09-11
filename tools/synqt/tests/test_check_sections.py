# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The top level of synqt.yaml: each section has one shape, and a key the tools do not read
is a mistake. Both are reported by name, before anything reads the sections.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest
import yaml

from synqt import check

EXAMPLE = Path(__file__).resolve().parents[3] / "examples" / "gavel"


def _config():
    return yaml.safe_load((EXAMPLE / "synqt.yaml").read_text())


@pytest.mark.parametrize("section, value", [
    ("project", "gavel"),
    ("entities", {"edge": 1}),
    ("connect_points", "edge"),
    ("scopes", ["anonymous", "user"]),
    ("security", [1]),
    ("identity", True),
    ("privacy", "yes"),
    ("build", "release"),
    ("monitoring", "monitor"),
    ("routes", {"path": "/"}),
    ("router", "/"),
    ("client", "app"),
    ("mesh", True),
    ("check", "strict"),
])
def test_a_section_of_the_wrong_shape_is_refused_by_name(section, value):
    config = _config()
    config[section] = value
    ok, messages = check.validate(config)
    assert not ok
    assert any(message.startswith("error:") and f"{section}:" in message
               for message in messages), messages


def test_a_key_the_tools_do_not_read_is_refused_by_name():
    # A typo, or a section written at the wrong level (`public:` belongs to the web edge
    # entity), would otherwise be ignored without a word.
    for key in ("secuirty", "public"):
        config = _config()
        config[key] = {"port": 443}
        ok, messages = check.validate(config)
        assert not ok
        assert any(message.startswith("error:") and f"'{key}'" in message
                   for message in messages), messages


def test_check_reports_a_wrong_shape_rather_than_failing(tmp_path):
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLE, project, ignore=shutil.ignore_patterns("build", "generated"))
    config = _config()
    config["scopes"] = ["anonymous", "user"]
    (project / "synqt.yaml").write_text(yaml.safe_dump(config))
    ok, messages = check.check_project(project)
    assert not ok
    assert any("scopes:" in message for message in messages), messages


def test_the_examples_use_only_sections_the_tools_read():
    for name in ("gavel", "arena", "plaza", "stall"):
        config = yaml.safe_load((EXAMPLE.parent / name / "synqt.yaml").read_text())
        ok, messages = check.validate(config)
        assert not any("top-level" in message for message in messages), (name, messages)


def test_a_misspelled_entity_key_is_refused_by_name():
    # `sharde: false` read as nothing would leave the entity shared across every caller.
    config = _config()
    edge = next(entity for entity in config["entities"] if entity["name"] == "edge")
    edge["sharde"] = False
    ok, messages = check.validate(config)
    assert not ok
    assert any("'sharde'" in message and "'edge'" in message for message in messages), messages


def test_a_misspelled_point_key_is_refused_by_name():
    # `scop: admin` read as nothing would leave the point open to every scope.
    config = _config()
    config["connect_points"][0]["scop"] = "admin"
    ok, messages = check.validate(config)
    assert not ok
    assert any("'scop'" in message for message in messages), messages


def test_a_project_point_cannot_call_itself_a_framework_point():
    # The tools mark the points they add with `framework:`, and the contract and export lints
    # skip those. Written by hand, it would take a project's own point out of them.
    config = _config()
    config["connect_points"][0]["framework"] = True
    ok, messages = check.validate(config)
    assert not ok
    assert any("framework" in message for message in messages), messages


def test_the_examples_use_only_keys_the_tools_read():
    for name in ("gavel", "arena", "plaza", "stall"):
        config = yaml.safe_load((EXAMPLE.parent / name / "synqt.yaml").read_text())
        ok, messages = check.validate(config)
        assert not any("nothing reads it" in message for message in messages), (name, messages)


def test_a_qt_version_this_synqt_does_not_build_is_refused():
    from synqt import toolchain

    config = _config()
    config["project"] = {"name": "shop", "qt_version": "6.11.1"}
    ok, messages = check.validate(config)
    assert not ok
    assert any("project.qt_version" in message and toolchain.QT_VERSION in message
               for message in messages), messages
    config["project"]["qt_version"] = toolchain.QT_VERSION
    _, messages = check.validate(config)
    assert not any("qt_version" in message for message in messages), messages


@pytest.mark.parametrize("name", ["my shop", "9lives", "shop;rm"])
def test_a_project_name_cmake_and_qml_cannot_use_is_refused(name):
    config = _config()
    config["project"]["name"] = name
    ok, messages = check.validate(config)
    assert not ok
    assert any("project.name" in message for message in messages), messages


@pytest.mark.parametrize("name", ["my shop", "9lives"])
def test_new_and_new_from_an_example_refuse_such_a_name_before_writing(tmp_path, name):
    from synqt import examples, newproject

    with pytest.raises(newproject.NewProjectError):
        newproject.scaffold(tmp_path, name)
    with pytest.raises(newproject.NewProjectError):
        examples.scaffold(tmp_path, name, "gavel")
    assert list(tmp_path.iterdir()) == []


def test_docker_names_the_compose_project_in_lowercase():
    from synqt import docker

    config = _config()
    config["project"]["name"] = "Shop"
    compose = docker.render_compose(config, docker.mesh_addresses(config))
    assert "name: shop\n" in compose
    assert "image: shop-synqt:latest" in compose


@pytest.mark.parametrize("key, value", [("same_site", "strict"), ("rotate", False)])
def test_a_session_setting_nothing_reads_is_refused(key, value):
    # SameSite follows origin_model and rotation is always on, so either line would only
    # look like it changed something.
    config = _config()
    config.setdefault("identity", {}).setdefault("session", {})[key] = value
    ok, messages = check.validate(config)
    assert not ok
    assert any(f"identity.session.{key}" in message for message in messages), messages


@pytest.mark.parametrize("section, key", [
    ("identity", "requred"),
    ("security", "max_conections_global"),
    ("mesh", "ca_cert"),
    ("project", "origin"),
])
def test_a_key_a_section_does_not_have_is_refused(section, key):
    # `identity.requred: true` read as nothing would leave sign-in optional.
    config = _config()
    config.setdefault(section, {})[key] = True
    ok, messages = check.validate(config)
    assert not ok
    assert any(f"{section}.{key}" in message for message in messages), messages


def test_every_documented_section_key_is_one_the_check_accepts():
    import re

    doc = (Path(__file__).resolve().parents[3] / "docs"
           / "project-layout-and-config.md").read_text(encoding="utf-8")
    for block in re.findall(r"```yaml\n(.*?)```", doc, re.S):
        try:
            parsed = yaml.safe_load(block)
        except yaml.YAMLError:
            continue
        if not isinstance(parsed, dict):
            continue
        assert check._section_key_messages(parsed) == [], block


@pytest.mark.parametrize("path", [("public", "trusted_proxy"), ("tls", "certfile"),
                                  ("mesh", "hots"), ("network", "inbond"),
                                  ("network", "inbound", "api_key")])
def test_a_key_an_entity_block_does_not_have_is_refused(path):
    # `public.trusted_proxy` read as nothing would leave every visitor counted as one address.
    config = _config()
    edge = next(entity for entity in config["entities"] if entity["name"] == "edge")
    block = edge
    for part in path[:-1]:
        block = block.setdefault(part, {})
    block[path[-1]] = "x"
    ok, messages = check.validate(config)
    assert not ok
    assert any(".".join(path) in message for message in messages), messages


def test_every_yaml_example_in_the_docs_uses_only_keys_the_check_accepts():
    import re

    docs = Path(__file__).resolve().parents[3] / "docs"
    for page in sorted(docs.glob("*.md")):
        for block in re.findall(r"```yaml\n(.*?)```", page.read_text(encoding="utf-8"), re.S):
            try:
                parsed = yaml.safe_load(block)
            except yaml.YAMLError:
                continue
            if not isinstance(parsed, dict):
                continue
            found = (check._section_key_messages(parsed)
                     + check._entity_block_key_messages(parsed) + check._key_messages(parsed))
            assert found == [], (page.name, found)
