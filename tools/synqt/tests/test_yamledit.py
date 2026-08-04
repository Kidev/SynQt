# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Configuration edits that leave everything they were not asked to change alone."""

from __future__ import annotations

import pytest
import yaml

from synqt import yamledit

SAMPLE = """\
# The project.
project:
  name: gavel

entities:
  # The browser.
  - name: client
    kind: client

  - name: web
    kind: service
    capability: web_edge

connect_points:
  - name: auction
    contract: Auction
    owner: web
    consumers: [client]
"""


def test_append_keeps_every_comment():
    out = yamledit.append_item(SAMPLE, "entities", {"name": "api", "kind": "service"})
    assert "# The project." in out
    assert "# The browser." in out
    assert yaml.safe_load(out)["entities"][-1] == {"name": "api", "kind": "service"}


def test_append_writes_its_own_comment_above_the_item():
    out = yamledit.append_item(SAMPLE, "connect_points",
                               {"name": "prices", "contract": "Prices", "owner": "api",
                                "consumers": ["web"]},
                               comment="Drawn in synqt design.")
    assert "# Drawn in synqt design." in out
    assert yaml.safe_load(out)["connect_points"][-1]["name"] == "prices"


def test_append_creates_a_list_the_file_does_not_have_yet():
    text = "entities:\n  - name: web\n    kind: service\n"
    out = yamledit.append_item(text, "connect_points",
                               {"name": "prices", "contract": "Prices", "owner": "web",
                                "consumers": ["client"]})
    assert yaml.safe_load(out)["connect_points"][0]["contract"] == "Prices"
    assert yaml.safe_load(out)["entities"][0]["name"] == "web"


def test_append_reaches_a_nested_list():
    text = "identity:\n  providers:\n    - name: github\n"
    out = yamledit.append_item(text, "identity.providers", {"name": "google"})
    assert [p["name"] for p in yaml.safe_load(out)["identity"]["providers"]] == [
        "github", "google"]


def test_patch_changes_one_field_and_nothing_else():
    out = yamledit.patch_item(SAMPLE, "connect_points", "auction",
                              {"consumers": ["client", "api"]})
    assert yaml.safe_load(out)["connect_points"][0]["consumers"] == ["client", "api"]
    assert out.count("contract: Auction") == 1
    assert "# The browser." in out


def test_patch_adds_a_field_the_item_did_not_have():
    out = yamledit.patch_item(SAMPLE, "connect_points", "auction",
                              {"instance": "per_session"})
    assert yaml.safe_load(out)["connect_points"][0]["instance"] == "per_session"


def test_patch_replaces_a_field_that_spans_several_lines():
    text = ("entities:\n"
            "  - name: db\n"
            "    provider:\n"
            "      name: sqlite\n"
            "      path: data/app.db\n"
            "    kind: service\n")
    out = yamledit.patch_item(text, "entities", "db", {"provider": {"name": "postgres"}})
    loaded = yaml.safe_load(out)["entities"][0]
    assert loaded["provider"] == {"name": "postgres"}
    assert loaded["kind"] == "service"


def test_remove_takes_the_item_and_its_leading_comment():
    out = yamledit.remove_item(SAMPLE, "entities", "client")
    assert "# The browser." not in out
    assert [e["name"] for e in yaml.safe_load(out)["entities"]] == ["web"]


def test_remove_of_the_last_item_leaves_an_empty_list_that_still_parses():
    text = "entities:\n  - name: only\n    kind: client\n"
    out = yamledit.remove_item(text, "entities", "only")
    assert yaml.safe_load(out)["entities"] == []


def test_remove_leaves_what_follows_the_list_alone():
    out = yamledit.remove_item(SAMPLE, "entities", "web")
    assert yaml.safe_load(out)["connect_points"][0]["name"] == "auction"
    assert "# The browser." in out


def test_set_scalar_reaches_a_nested_path():
    out = yamledit.set_scalar(SAMPLE, "project.name", "renamed")
    assert yaml.safe_load(out)["project"]["name"] == "renamed"


def test_set_scalar_adds_a_key_the_parent_did_not_have():
    out = yamledit.set_scalar(SAMPLE, "project.origin_model", "same_origin")
    assert yaml.safe_load(out)["project"]["origin_model"] == "same_origin"
    assert yaml.safe_load(out)["project"]["name"] == "gavel"


def test_set_scalar_can_write_a_whole_section_at_the_root():
    out = yamledit.set_scalar(SAMPLE, "identity", {"provider": "github",
                                                   "required": False})
    assert yaml.safe_load(out)["identity"] == {"provider": "github", "required": False}
    assert "# The browser." in out


def test_an_unknown_item_is_refused_rather_than_appended():
    with pytest.raises(yamledit.YamlEditError):
        yamledit.patch_item(SAMPLE, "entities", "nobody", {"kind": "service"})


def test_removing_an_unknown_item_is_refused_too():
    with pytest.raises(yamledit.YamlEditError):
        yamledit.remove_item(SAMPLE, "entities", "nobody")


def test_a_shape_it_cannot_edit_textually_is_refused_loudly():
    # A flow-style list of mappings is legal YAML and not a shape this project writes.
    text = "entities: [{name: client, kind: client}]\n"
    with pytest.raises(yamledit.YamlEditError):
        yamledit.append_item(text, "entities", {"name": "web", "kind": "service"})


def test_a_list_of_scalars_is_not_a_list_of_items():
    text = "entities:\n  - client\n  - web\n"
    with pytest.raises(yamledit.YamlEditError):
        yamledit.append_item(text, "entities", {"name": "api"})


def test_a_path_through_something_that_is_not_a_mapping_is_refused():
    with pytest.raises(yamledit.YamlEditError):
        yamledit.set_scalar(SAMPLE, "project.name.deeper", 1)


def test_every_edit_leaves_a_document_that_parses_to_the_expected_object():
    out = yamledit.append_item(SAMPLE, "entities", {"name": "api", "kind": "service"})
    out = yamledit.patch_item(out, "entities", "api", {"blueprint": "persistence"})
    out = yamledit.remove_item(out, "entities", "client")
    loaded = yaml.safe_load(out)
    assert [e["name"] for e in loaded["entities"]] == ["web", "api"]
    assert loaded["entities"][-1]["blueprint"] == "persistence"


def test_the_rest_of_the_document_is_byte_for_byte_what_it_was():
    out = yamledit.patch_item(SAMPLE, "connect_points", "auction", {"instance": "shared"})
    before, after = SAMPLE.split("connect_points:")[0], out.split("connect_points:")[0]
    assert before == after
