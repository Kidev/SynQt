# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Unit tests for the SynQt docs lexers.

Run with: python3 -m pytest tools/pygments-synqt/tests

Covers the token decisions the docs colours rely on, including the runtime accessors and the
CSS classes docs/stylesheets/extra.css targets by name.
"""

import os
import sys

from pygments.formatters.html import _get_ttype_class
from pygments.token import Comment, Name, String

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "src"))

from synqt_pygments.lexers import SynqtQmlLexer  # noqa: E402


def tokens(source):
    """Every non-blank token in `source`, as (token type, text) pairs."""
    return [(kind, text) for kind, text in SynqtQmlLexer().get_tokens(source) if text.strip()]


def kind_of(source, word):
    """The token type the lexer gives `word` in `source`."""
    return next(kind for kind, text in tokens(source) if text == word)


def test_accessor_class_is_the_one_the_stylesheet_targets():
    # extra.css colours `.nb-Accessor`, the class Pygments derives from this token.
    assert _get_ttype_class(Name.Builtin.Accessor) == "nb-Accessor"


def test_every_documented_accessor_is_one():
    # The list in runtime-api.md, each addressed the way that page shows it.
    source = """
    Item {
        Component.onCompleted: {
            Server.feed.reload();
            Session.login();
            Router.go("/home");
            App.applyUpdate();
            Caller.hasScope("user");
            Client.emitDone();
        }
    }
    """
    for accessor in ("App", "Caller", "Client", "Router", "Server", "Session"):
        assert kind_of(source, accessor) is Name.Builtin.Accessor, accessor


def test_an_accessor_is_one_wherever_it_stands():
    # As a binding, an argument, and an attached handler target: one colour.
    source = """
    Item {
        target: Server.auth
        Session.onStateChanged: report(Session)
    }
    """
    assert [kind for kind, text in tokens(source) if text == "Session"] == \
        [Name.Builtin.Accessor, Name.Builtin.Accessor]
    assert kind_of(source, "Server") is Name.Builtin.Accessor


def test_a_name_that_merely_contains_an_accessor_is_not_one():
    # `SessionSource` and `SynClient` are not accessors.
    source = """
    SessionSource {
        property var client: SynClient
        prop: ServerSide.value
    }
    """
    assert kind_of(source, "SessionSource") is not Name.Builtin.Accessor
    assert kind_of(source, "SynClient") is not Name.Builtin.Accessor
    assert kind_of(source, "ServerSide") is Name.Class


def test_project_types_and_javascript_globals_keep_their_own_colors():
    source = """
    ApplicationWindow {
        Text { text: Database.items.count + Math.round(1.5) }
    }
    """
    assert kind_of(source, "ApplicationWindow") is Name.Class
    assert kind_of(source, "Database") is Name.Class
    assert kind_of(source, "Math") is Name.Builtin


def test_an_accessor_named_in_prose_or_in_a_string_is_left_alone():
    source = """
    Item {
        // Caller is the accessor
        text: "Server"
    }
    """
    kinds = dict((text, kind) for kind, text in tokens(source))
    assert kinds["// Caller is the accessor\n"] is Comment.Single
    assert kinds['"Server"'] is String.Double


# The contract lexer, and the configuration that carries a contract

from pygments.token import Keyword, Number, Punctuation  # noqa: E402
from synqt_pygments.lexers import SynLexer, SynqtYamlLexer  # noqa: E402


def contract_tokens(source, lexer=None):
    """Every non-blank token in `source`, as (token type, text) pairs."""
    return [(kind, text)
            for kind, text in (lexer or SynLexer()).get_tokens(source) if text.strip()]


def test_a_member_line_is_four_things_and_not_one():
    # Kind, type, width and name are separate tokens.
    found = contract_tokens("prop string[80] itemName\n")
    assert found == [
        (Keyword, "prop"),
        (Keyword.Type.Contract, "string"),
        (Punctuation, "["),
        (Number.Integer.Width, "80"),
        (Punctuation, "]"),
        (Name.Function, "itemName"),
    ]


def test_the_two_classes_the_stylesheet_targets():
    # extra.css colours `.kt-Contract` and `.mi-Width`, classes of their own.
    assert _get_ttype_class(Keyword.Type.Contract) == "kt-Contract"
    assert _get_ttype_class(Number.Integer.Width) == "mi-Width"


def test_a_slot_names_its_member_whether_or_not_it_answers():
    # The member name in `slot load()` and in `slot bool allows(...)`.
    assert (Name.Function, "load") in contract_tokens("slot load()\n")
    answering = contract_tokens("slot bool allows(string[64] sub)\n")
    assert (Keyword.Type.Contract, "bool") in answering
    assert (Name.Function, "allows") in answering
    assert (Name.Variable, "sub") in answering


def test_the_export_block_of_a_connect_point_is_read_as_a_contract():
    # The block is found by its `export:` key: contract tokens inside, plain YAML around.
    found = contract_tokens("""entities:
  - name: edge
    type: web_edge
connect_points:
  - name: feed
    owner: edge
    export: |
      prop bool loaded
      model rows(int id, string[120] title)
  - name: other
    owner: edge
""", SynqtYamlLexer())
    assert (Keyword, "prop") in found
    assert (Keyword.Type.Contract, "bool") in found
    assert (Number.Integer.Width, "120") in found
    assert (Name.Function, "rows") in found
    # And the key after the block ends it. `other` is a value, not a member name.
    assert (Name.Function, "other") not in found
    assert any(kind in String or "Scalar" in str(kind) for kind, text in found
               if text == "other")


def test_a_value_that_looks_like_a_member_elsewhere_is_left_alone():
    # `prop int count` under another key stays a string.
    found = contract_tokens("note: prop int count\n", SynqtYamlLexer())
    assert (Keyword, "prop") not in found
