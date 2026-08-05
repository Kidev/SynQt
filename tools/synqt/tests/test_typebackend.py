# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Two ways to answer what type an expression has, held to one fixture set."""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest

from synqt import typebackend

# (QML, expression, the type a backend may not get wrong)
FIXTURES = [
    ("function f() { g(1); }", "1", "int"),
    ("function f() { g(1.5); }", "1.5", "real"),
    ("function f() { g(\"a\"); }", '"a"', "string"),
    ("function f() { const w = { id: \"a\", n: 2 }; g(w.id); }", "w.id", "string"),
    ("function f() { const w = { id: \"a\", n: 2 }; g(w.n); }", "w.n", "int"),
    ("function f() { const w = pick(); g(w.id); }", "w.id", "var"),
]


def _run(backend, fixtures):
    """Ask one backend for the type of each fixture's expression, the way `infer` does.

    Each snippet becomes a QML file of its own and every question is asked in one batch,
    because a backend that follows a value back to where it was built has to be handed the
    file it was built in, and starting a process per expression would be the slow way to
    ask the same thing.
    """
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for position, (qml, _, _) in enumerate(fixtures):
            (root / f"F{position}.qml").write_text(f"Item {{\n    {qml}\n}}\n",
                                                   encoding="utf-8")
        queries = [typebackend.Query(expression, f"F{position}.qml", 2)
                   for position, (_, expression, _) in enumerate(fixtures)]
        return backend.types(queries, typebackend.extract(root))


def _ask(backend, qml, expression, line):
    """One expression in one whole QML file, asked for by where it was written."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "A.qml").write_text(qml, encoding="utf-8")
        query = typebackend.Query(expression, "A.qml", line)
        return backend.types([query], typebackend.extract(root))[0]


def test_the_heuristic_types_literals_and_answers_var_otherwise():
    backend = typebackend.HeuristicBackend()
    answers = _run(backend, FIXTURES)
    assert answers[0].type == "int"
    assert answers[3].type == "var"          # it cannot follow the assignment
    assert answers[3].certain is False


@pytest.mark.skipif(not typebackend.available(), reason="node and ts-morph are not here")
def test_typescript_follows_the_value_back_to_where_it_was_built():
    answers = _run(typebackend.TsBackend(), FIXTURES)
    assert answers[3].type == "string"
    assert answers[4].type == "int"
    assert answers[3].certain is True


@pytest.mark.skipif(not typebackend.available(), reason="node and ts-morph are not here")
def test_neither_backend_answers_a_wrong_type():
    for backend in (typebackend.HeuristicBackend(), typebackend.TsBackend()):
        for answer, (_, _, expected) in zip(_run(backend, FIXTURES), FIXTURES):
            assert answer.type in (expected, "var"), f"{backend} said {answer.type}"


@pytest.mark.skipif(not typebackend.available(), reason="node and ts-morph are not here")
def test_a_property_the_file_declares_keeps_the_type_it_was_declared_with():
    # `real` and `int` are one type in JavaScript, so the extraction has to carry the
    # difference over itself; a coordinate coming back as a count would be a wrong answer,
    # not a missing one.
    qml = ("Item {\n"
           "    id: root\n"
           "    property real aimX: 0\n"
           "    property int lives: 3\n"
           "    function send() { g(root.aimX, root.lives); }\n"
           "}\n")
    assert _ask(typebackend.TsBackend(), qml, "root.aimX", 5).type == "real"
    assert _ask(typebackend.TsBackend(), qml, "root.lives", 5).type == "int"


@pytest.mark.skipif(not typebackend.available(), reason="node and ts-morph are not here")
def test_a_longer_name_beginning_the_same_way_is_not_the_expression():
    # `auction.highBid` is also how `auction.highBidder` starts, and the first text that
    # matches is not always the expression that was asked about.
    qml = ("Item {\n"
           "    id: auction\n"
           "    property string highBidder: \"nobody yet\"\n"
           "    property int highBid: 0\n"
           "    function close() { g(auction.highBidder, auction.highBid); }\n"
           "}\n")
    assert _ask(typebackend.TsBackend(), qml, "auction.highBid", 5).type == "int"


def test_auto_falls_back_and_says_which_it_used(tmp_path, monkeypatch):
    monkeypatch.setattr(typebackend, "available", lambda: False)
    backend = typebackend.resolve("auto", tmp_path)
    assert isinstance(backend, typebackend.HeuristicBackend)


def test_ts_refuses_rather_than_falling_back(tmp_path, monkeypatch):
    monkeypatch.setattr(typebackend, "available", lambda: False)
    with pytest.raises(typebackend.TypeBackendError) as caught:
        typebackend.resolve("ts", tmp_path)
    assert "ts-morph" in str(caught.value)


def test_extraction_keeps_a_line_marker_back_to_the_qml(tmp_path):
    qml = "Item {\n    function f() {\n        g(1);\n    }\n}\n"
    (tmp_path / "A.qml").write_text(qml)
    extracted = dict(typebackend.extract(tmp_path))
    assert "A.qml:2" in extracted["A.qml"]


def test_a_binding_expression_is_extracted_as_well_as_a_function_body(tmp_path):
    qml = 'Item { property int n: 1 + 2\n    onClicked: g("a") }\n'
    (tmp_path / "A.qml").write_text(qml)
    body = dict(typebackend.extract(tmp_path))["A.qml"]
    assert "1 + 2" in body and 'g("a")' in body
