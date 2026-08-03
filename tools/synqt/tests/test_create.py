# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt create`` asks the questions ``synqt new`` takes as flags.

The two commands have to stay one scaffolder with two front ends, so the end-to-end test
here asserts that answering the questions produces exactly what the equivalent flags
produce. The rest pins the refusals: no terminal, and an answer that names nothing.
"""

import io
import shutil
import tempfile
from pathlib import Path

import pytest
import yaml

from synqt import create, newproject


def _answers(*lines):
    """A stream that answers the questions in order."""
    return io.StringIO("".join(f"{line}\n" for line in lines))


def test_create_refuses_without_a_terminal():
    # Degrading to defaults would scaffold a project nobody chose, and the missing entity
    # would surface much later. It names the scriptable command instead.
    with pytest.raises(create.CreateError) as raised:
        create.create(".", name="app", source=io.StringIO(""), out=io.StringIO())
    assert "synqt new" in str(raised.value)


def test_empty_answers_take_the_defaults():
    chosen = create.answers(io.StringIO(), _answers("", "", ""))
    assert chosen == {"name": "my-app", "auth": None, "blueprints": []}


def test_a_name_is_one_directory_component():
    out = io.StringIO()
    # Two refusals, then an acceptable one: a path and a hidden name are not names.
    assert create.ask_name(out, _answers("some/where", ".hidden", "shop")) == "shop"
    assert out.getvalue().count("one directory component") == 2


def test_a_given_name_is_not_asked_for_again():
    chosen = create.answers(io.StringIO(), _answers("", ""), name="given")
    assert chosen["name"] == "given"


@pytest.mark.parametrize("answer", ["", "none", "no", "n", "NONE"])
def test_auth_defaults_to_none(answer):
    assert create.ask_auth(io.StringIO(), _answers(answer)) is None


def test_auth_accepts_a_templated_provider():
    assert create.ask_auth(io.StringIO(), _answers("github")) == "github"


def test_an_untemplated_provider_is_taken_and_flagged():
    # Not a refusal: a provider with no template is configured by hand afterwards.
    out = io.StringIO()
    assert create.ask_auth(out, _answers("keycloak")) == "keycloak"
    assert "no template" in out.getvalue()


def test_blueprints_are_parsed_trimmed_and_deduplicated():
    chosen = create.ask_blueprints(io.StringIO(), _answers(" persistence , cache ,persistence"))
    assert chosen == ["persistence", "cache"]


def test_an_unknown_blueprint_names_the_ones_that_exist():
    with pytest.raises(create.CreateError) as raised:
        create.ask_blueprints(io.StringIO(), _answers("postgres"))
    assert "persistence" in str(raised.value)


def test_every_offered_blueprint_is_one_addentity_knows():
    # The menu is a hand-written subset, so it can drift from the scaffolder it feeds.
    from synqt import addentity
    for blueprint in create._STARTING_BLUEPRINTS:
        assert blueprint in addentity.BLUEPRINTS
        assert blueprint in create._BLUEPRINT_BLURB


def test_input_ending_mid_question_stops_rather_than_guessing():
    with pytest.raises(create.CreateError) as raised:
        create.answers(io.StringIO(), _answers("shop"))
    assert "input ended" in str(raised.value)


def test_answering_the_questions_matches_the_equivalent_flags():
    asked = Path(tempfile.mkdtemp())
    flagged = Path(tempfile.mkdtemp())
    try:
        create.create(asked, out=io.StringIO(), interactive=True,
                      source=_answers("shop", "github", "persistence"))
        newproject.scaffold(flagged, "shop", auth="github", blueprints=["persistence"])

        asked_config = yaml.safe_load((asked / "shop" / "synqt.yaml").read_text())
        flagged_config = yaml.safe_load((flagged / "shop" / "synqt.yaml").read_text())
        assert asked_config == flagged_config

        asked_files = sorted(p.relative_to(asked) for p in (asked / "shop").rglob("*"))
        flagged_files = sorted(p.relative_to(flagged) for p in (flagged / "shop").rglob("*"))
        assert asked_files == flagged_files
    finally:
        shutil.rmtree(asked, ignore_errors=True)
        shutil.rmtree(flagged, ignore_errors=True)
