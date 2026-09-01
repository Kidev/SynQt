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
