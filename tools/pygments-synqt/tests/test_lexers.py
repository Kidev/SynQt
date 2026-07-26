# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Unit tests for the SynQt docs lexers.

Run with: python3 -m pytest tools/pygments-synqt/tests

These cover the token decisions the docs' colors are built on, in particular the
runtime accessors: which names are one, which lookalikes are not, and the CSS class
Pygments writes for them, which docs/stylesheets/extra.css targets by name.
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
    # extra.css colors `.nb-Accessor`. Pygments derives that class from the token by
    # walking up to the nearest one it knows, so this is the pairing to hold on to: a
    # rename on either side and the accessors silently go back to the default color.
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
    # Bound to a property, passed as an argument, and as the target of an attached
    # handler: the same object each time, so the same color each time.
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
    # `SessionSource` is a generated type and `SynClient` is a C++ class; neither is the
    # accessor whose name they happen to spell part of.
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
