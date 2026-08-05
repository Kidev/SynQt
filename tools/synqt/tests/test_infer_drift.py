# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A contract and the QML that uses it, compared."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest
import yaml

from synqt import check as checkmod
from synqt import typebackend

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _copy(tmp_path, name):
    target = tmp_path / name
    shutil.copytree(EXAMPLES / name, target,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    return target


def _config(project):
    return yaml.safe_load((project / "synqt.yaml").read_text())


def test_the_examples_have_no_drift(tmp_path):
    for name in ("gavel", "arena"):
        project = _copy(tmp_path, name)
        assert checkmod.lint_contract_drift(_config(project), project) == []


def test_a_slot_the_qml_calls_and_the_contract_lacks_is_an_error(tmp_path):
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "Main.qml"
    main.write_text(main.read_text().replace(
        "Server.auction.placeBid(", "Server.auction.placeBidNow("))
    messages = checkmod.lint_contract_drift(_config(project), project)
    assert any(m.startswith("error:") and "placeBidNow" in m for m in messages)


def test_a_declared_member_nobody_uses_is_a_note(tmp_path):
    project = _copy(tmp_path, "gavel")
    syn = project / "shared" / "Auction.syn"
    syn.write_text(syn.read_text().replace("}", "    prop int unused\n}"))
    messages = checkmod.lint_contract_drift(_config(project), project)
    assert any(m.startswith("note:") and "unused" in m for m in messages)


def test_dynamic_access_suppresses_the_unused_note_for_that_point(tmp_path):
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "Main.qml"
    main.write_text(main.read_text()
                    + "\nQtObject { property var v: Server[n].x }\n")
    syn = project / "shared" / "Auction.syn"
    syn.write_text(syn.read_text().replace("}", "    prop int unused\n}"))
    assert not any("unused" in m for m in
                   checkmod.lint_contract_drift(_config(project), project))


@pytest.mark.skipif(not typebackend.available(), reason="node and ts-morph are not here")
def test_an_argument_of_the_wrong_type_at_a_connect_point_is_an_error(tmp_path):
    # placeBid is declared `slot placeBid(int amount)`; the client hands it a string.
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "Main.qml"
    main.write_text(main.read_text().replace(
        "Server.auction.placeBid(parseInt(amountField.text))",
        'Server.auction.placeBid("abc")'))
    messages = checkmod.lint_contract_drift(_config(project), project, types="ts")
    assert any(m.startswith("error:") and "placeBid" in m and "amount" in m
               for m in messages)


def test_an_uncertain_argument_type_is_not_an_error(tmp_path):
    # The heuristic cannot type an expression, so it must stay silent rather than guess.
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "Main.qml"
    main.write_text(main.read_text().replace(
        "Server.auction.placeBid(parseInt(amountField.text))",
        "Server.auction.placeBid(somethingOpaque())"))
    assert not any(m.startswith("error:") for m in
                   checkmod.lint_contract_drift(_config(project), project,
                                                types="heuristic"))


def test_only_connect_point_calls_are_type_checked(tmp_path):
    # A plain QML call with a mistyped argument is nobody's business here.
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "Main.qml"
    main.write_text(main.read_text() + "\nQtObject {\n"
                    "    function local(n: int) { return n + 1; }\n"
                    '    Component.onCompleted: local("not a number")\n}\n')
    assert not any(m.startswith("error:") for m in
                   checkmod.lint_contract_drift(_config(project), project))


def test_a_member_used_only_by_the_owner_is_not_drift(tmp_path):
    # `property var store: []` on a Source is the entity's own state. The scan offers it
    # as a contract member when it is writing one; it is not evidence anybody expects it
    # to cross a link, and reporting it would fail every correct project here.
    project = _copy(tmp_path, "gavel")
    assert not any("store" in m for m in
                   checkmod.lint_contract_drift(_config(project), project))


def test_check_project_reports_drift(tmp_path):
    project = _copy(tmp_path, "gavel")
    ok, messages = checkmod.check_project(project)
    assert ok
    assert isinstance(messages, list)
