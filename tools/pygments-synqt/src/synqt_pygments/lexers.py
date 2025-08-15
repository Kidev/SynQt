# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
"""Pygments lexers used by the SynQt docs.

SynLexer highlights `.syn` contract files (the `contract`/`record` surface over
QtRO rep files, see docs/programming-model.md). CliLexer highlights the `synqt`
CLI reference listings (command, placeholders, flags, and a trailing `#`
description), used for the long command tables in docs/build-system-and-cli.md
and docs/providers.md so a command reads apart from its description at a
glance, the same job a shell prompt/comment split does for real shell examples.

SynqtQmlLexer highlights the QML code blocks. It extends Pygments' stock QmlLexer
so a SynQt attached signal handler, `Contract.onSignal:` (see the "Handling a
connect point's signals" section of docs/programming-model.md), colors the
contract type and the handler separately instead of as one keyword blob. The
docs map the `qml` fence to it via `extend_pygments_lang` in mkdocs.yml, so
````qml` blocks match the hand-authored home page tooltip.
"""

from pygments.lexer import RegexLexer, bygroups, inherit, words
from pygments.lexers.webmisc import QmlLexer
from pygments.token import Comment, Keyword, Name, Operator, Punctuation, Text, Whitespace

__all__ = ["SynLexer", "CliLexer", "SynqtQmlLexer"]


class SynLexer(RegexLexer):
    """Lexer for SynQt `.syn` contract files."""

    name = "SynQt Contract"
    aliases = ["syn", "synqt-contract"]
    filenames = ["*.syn"]
    mimetypes = ["text/x-synqt-contract"]

    keywords = ("contract", "record", "prop", "model", "signal", "slot")
    builtin_types = ("int", "string", "bool", "float", "double")

    tokens = {
        "root": [
            (r"//.*$", Comment.Single),
            (r"\s+", Whitespace),
            (r"[{}()]", Punctuation),
            (r",", Punctuation),
            (words(keywords, suffix=r"\b"), Keyword),
            (words(builtin_types, suffix=r"\b"), Keyword.Type),
            # A capitalized identifier is a contract or record type, whether it is
            # being declared (`contract Todo`) or referenced as a parameter or
            # return type (`slot insert(ItemRow row)`).
            (r"[A-Z][A-Za-z0-9_]*", Name.Class),
            (r"[a-z_][A-Za-z0-9_]*", Name),
            (r".", Text),
        ],
    }


class SynqtQmlLexer(QmlLexer):
    """QML lexer that colors type names, and understands SynQt's attached handlers.

    Two things the stock QmlLexer gets wrong for these docs.

    A type being instantiated (`ApplicationWindow {`, `ListView {`) falls through
    to the JavaScript lexer's catch-all identifier rule, so it arrives as plain
    text: the one word that says what an object *is* looks like every local
    variable around it. It is the same thing `contract Feed` names in a `.syn`
    file, so it gets the same token, `Name.Class`, and therefore the same color.

    And any `identifier.chain:` binding is matched as a single Keyword token, so
    `Auth.onLoginFailed:` colors as one blob and the contract type is lost.
    Splitting the leading `Type.` from the `on<Signal>:` handler restores it. A
    plain handler like `onClicked:` has no `Type.` prefix and still falls through
    to the inherited rule, so nothing else changes.
    """

    name = "SynQt QML"
    aliases = ["synqt-qml"]
    filenames = []
    mimetypes = []

    tokens = {
        "root": [
            # A type being instantiated: the name immediately before the brace that
            # opens the object, `ApplicationWindow {` or `QtObject {`, optionally
            # qualified (`Qt.labs.settings.Settings {`). The brace must be on the
            # same line, which is how QML is written throughout these docs and what
            # keeps the rule from reaching across a line to an unrelated block.
            (r"([A-Z]\w*(?:\.[A-Z]\w*)*)([ \t]*)(\{)",
             bygroups(Name.Class, Whitespace, Punctuation)),
            # `Behavior on width { ... }`: the same declaration with the property it
            # animates wedged into the middle of it.
            (r"(Behavior)(\s+)(on)(\s+)(\w+)([ \t]*)(\{)",
             bygroups(Name.Class, Whitespace, Keyword, Whitespace, Name, Whitespace,
                      Punctuation)),
            # An attached signal handler: SynQt's `Auth.onLoginFailed:`, and QML's own
            # `Component.onCompleted:`. Split the type from the handler so the type is
            # not swallowed into the binding keyword.
            (r"([A-Z]\w*)(\.)(on[A-Z]\w*\s*:)", bygroups(Name.Class, Punctuation, Keyword)),
            # An arrow reads as one operator, not `=` then `>`.
            (r"=>", Operator),
            inherit,
        ],
    }


class CliLexer(RegexLexer):
    """Lexer for the `synqt` CLI reference listings in the docs."""

    name = "SynQt CLI Reference"
    aliases = ["cli", "synqt-cli"]
    filenames = []
    mimetypes = []

    tokens = {
        "root": [
            (r"#.*$", Comment.Single),
            (r"\s+", Whitespace),
            (r"\.\.\.", Operator),
            (r"\|", Operator),
            (r"[\[\]]", Punctuation),
            (r"<[^>]+>", Name.Variable),
            (r"--?[A-Za-z][\w-]*", Name.Attribute),
            (r"synqt\b", Name.Builtin),
            (r"[A-Za-z][\w-]*", Keyword),
            (r".", Text),
        ],
    }
