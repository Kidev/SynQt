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
    """QML lexer that understands SynQt's attached signal handlers.

    The stock QmlLexer matches any `identifier.chain:` binding as a single
    Keyword token, so `Auth.onLoginFailed:` colors as one blob and the contract
    type is lost. Prepending a rule that splits the leading `Type.` from the
    `on<Signal>:` handler restores the type, matching the home page tooltip. A
    plain handler like `onClicked:` has no `Type.` prefix and still falls through
    to the inherited rule, so nothing else changes.
    """

    name = "SynQt QML"
    aliases = ["synqt-qml"]
    filenames = []
    mimetypes = []

    tokens = {
        "root": [
            # A SynQt attached signal handler: `Auth.onLoginFailed:`. Split the
            # contract type (Name) from the handler (Keyword) so the type is not
            # swallowed into the binding keyword.
            (r"([A-Z]\w*)(\.)(on[A-Z]\w*\s*:)", bygroups(Name.Other, Punctuation, Keyword)),
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
