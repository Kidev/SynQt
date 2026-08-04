# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The QML tokenizer both the graphics scan and the contract inference read with."""

from __future__ import annotations

from synqt import qmlscan


def _texts(source):
    return [token.text for token in qmlscan.tokenize(source)]


def _kinds(source):
    return [token.kind for token in qmlscan.tokenize(source)]


def test_comments_and_line_terminators_match_the_lexer():
    # A lone "\r" ends a statement; "//" runs to it; "/* */" spans lines.
    assert qmlscan.stripped("a // one\rb /* two\nthree */ c") == "a \nb  c"


def test_byte_order_mark_is_skipped():
    # Written as an escape, not the character itself: every tracked file here is ASCII,
    # and an invisible byte order mark in the test about byte order marks reads as a typo.
    assert qmlscan.stripped("\ufeffItem {}") == "Item {}"


def test_a_semicolon_survives_stripping_because_it_ends_a_statement():
    assert qmlscan.stripped("import QtQuick; Item {}") == "import QtQuick; Item {}"


def test_identifiers_and_punctuation_are_separate_tokens():
    assert _texts("Database.scores.award(a, b)") == [
        "Database", ".", "scores", ".", "award", "(", "a", ",", "b", ")"]


def test_a_string_literal_is_one_token_that_keeps_its_kind():
    tokens = qmlscan.tokenize('emit("hello")')
    assert [token.kind for token in tokens] == ["ident", "punct", "string", "punct"]
    assert qmlscan.literal_type(tokens[2]) == "string"


def test_a_template_literal_is_a_string_too():
    assert _kinds("`a ${b} c`") == ["string"]


def test_an_escaped_quote_does_not_end_the_string():
    assert _texts(r'f("a\"b", 1)') == ["f", "(", r'"a\"b"', ",", "1", ")"]


def test_numbers_separate_int_from_real():
    tokens = qmlscan.tokenize("f(1, 1.5)")
    assert qmlscan.literal_type(tokens[2]) == "int"
    assert qmlscan.literal_type(tokens[4]) == "real"


def test_an_exponent_and_a_hexadecimal_are_read_as_numbers():
    assert _kinds("f(1e3, 0x1f)") == ["ident", "punct", "real", "punct", "int", "punct"]


def test_booleans_are_their_own_kind():
    assert qmlscan.literal_type(qmlscan.tokenize("f(true)")[2]) == "bool"


def test_null_carries_no_type_of_its_own():
    tokens = qmlscan.tokenize("f(null)")
    assert tokens[2].kind == "null"
    assert qmlscan.literal_type(tokens[2]) == "var"


def test_an_identifier_is_not_a_literal_so_its_type_is_unknown():
    assert qmlscan.literal_type(qmlscan.tokenize("f(other)")[2]) == "var"


def test_a_token_carries_the_line_it_came_from():
    assert [token.line for token in qmlscan.tokenize("a\nb\rc")] == [1, 2, 3]


def test_a_block_comment_still_advances_the_line_count():
    assert [token.line for token in qmlscan.tokenize("a /* x\ny */ b")] == [1, 2]


def test_a_comment_holds_no_tokens():
    assert _texts("// Database.scores.award(x)\nItem") == ["Item"]


def test_a_string_holds_no_tokens():
    assert _texts('"Database.scores.award(x)"')[0] != "Database"


def test_an_unterminated_string_ends_at_the_end_of_the_file():
    assert _kinds('f("oops') == ["ident", "punct", "string"]


def test_every_kind_the_tokenizer_emits_is_declared():
    source = 'a . "s" 1 1.5 true null'
    assert set(_kinds(source)) <= set(qmlscan.KINDS)


def test_the_root_type_is_the_name_in_front_of_the_first_brace():
    assert qmlscan.root_type("import QtQuick\n\nItemsSource {\n    id: root\n}\n") \
        == "ItemsSource"


def test_a_dotted_root_type_comes_back_whole():
    assert qmlscan.root_type("import QtQuick\n\nQtQuick.Item {\n}\n") == "QtQuick.Item"


def test_a_root_type_named_in_a_comment_is_not_the_root():
    source = "// ItemsSource is what this should be.\nimport QtQuick\n\nQtObject {\n}\n"
    assert qmlscan.root_type(source) == "QtObject"


def test_a_pragma_and_a_carriage_return_import_do_not_hide_the_root():
    """A `\\r` alone ends a line for the QML lexer, and a file written on an old Mac or by
    a careless tool still has a root object. A line-based reading sees one long line and
    finds nothing, which would turn an error into silence."""
    assert qmlscan.root_type("pragma Singleton\rimport QtQuick\rWorld {\r}\r") == "World"


def test_a_file_with_no_object_in_it_has_no_root_type():
    assert qmlscan.root_type("pragma Singleton\nimport QtQuick\n") is None
