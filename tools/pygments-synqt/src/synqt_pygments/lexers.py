# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
"""Pygments lexers used by the SynQt docs.

SynLexer highlights `.syn` contracts (docs/programming-model.md). CliLexer highlights the
`synqt` CLI reference listings (command, placeholders, flags and a trailing `#` description)
in docs/build-system-and-cli.md and docs/providers.md. SynqtQmlLexer extends Pygments'
QmlLexer so an attached handler `Contract.onSignal:` colors the type and the handler
separately; mkdocs.yml maps the `qml` fence to it with `extend_pygments_lang`.
"""

from pygments.lexer import RegexLexer, bygroups, default, inherit, words
from pygments.lexers.data import YamlLexer
from pygments.lexers.webmisc import QmlLexer
from pygments.token import (Comment, Keyword, Name, Number, Operator, Punctuation,
                            Text, Whitespace)

__all__ = ["SynLexer", "CliLexer", "SynqtQmlLexer", "SynqtYamlLexer"]


class SynLexer(RegexLexer):
    """Lexer for SynQt contract members: a `.syn` file, or the `export:` block of a connect
    point.

    Colors four things apart: the member kind (`prop`, `model`, `signal`, `slot`), each
    type, its bound (`string[80]`), and the names.
    """

    name = "SynQt Contract"
    aliases = ["syn", "synqt-contract"]
    filenames = ["*.syn"]
    mimetypes = ["text/x-synqt-contract"]

    keywords = ("contract", "record")
    member_kinds = ("prop", "model", "signal", "slot")
    # QML's value types, the whole contract vocabulary (docs/programming-model.md).
    builtin_types = ("int", "string", "bool", "real", "float", "double", "var", "url",
                     "date", "color", "point", "size", "rect")

    # Custom tokens: Pygments writes an unknown leaf as its own CSS class (`kt-Contract`,
    # `mi-Width`), so the docs can color contract types and bounds without changing shared
    # classes. Material paints `.k` and `.kt` alike.
    contract_type = Keyword.Type.Contract
    width = Number.Integer.Width

    # A type with its bound; brackets and number are separate tokens.
    sized_type = (r"\b([A-Za-z_]\w*)(\[)(\d+)(\])",
                  bygroups(contract_type, Punctuation, width, Punctuation))
    plain_type = (words(builtin_types, prefix=r"\b", suffix=r"\b"), contract_type)
    # A capitalized identifier is a contract or record type, declared (`contract Todo`) or
    # referenced (`slot insert(ItemRow row)`).
    named_type = (r"[A-Z][A-Za-z0-9_]*", Name.Class)

    tokens = {
        "root": [
            (r"//.*$", Comment.Single),
            (r"\s+", Whitespace),
            (r"[{}()]", Punctuation),
            (r",", Punctuation),
            (words(keywords, suffix=r"\b"), Keyword),
            # Each member kind moves to a state that reads the type (if any), then the name.
            (r"\b(prop)\b", Keyword, "member"),
            (r"\b(model|signal)\b", Keyword, "member"),
            (r"\b(slot)\b", Keyword, "member"),
            sized_type,
            plain_type,
            named_type,
            (r"[a-z_][A-Za-z0-9_]*", Name.Variable),
            (r".", Text),
        ],
        # Between a member kind and its name: zero or more type tokens, then the name.
        "member": [
            (r"[ \t]+", Whitespace),
            sized_type,
            plain_type,
            named_type,
            (r"[a-z_][A-Za-z0-9_]*", Name.Function, "#pop"),
            default("#pop"),
        ],
    }


class SynqtQmlLexer(QmlLexer):
    """QML lexer that colors type names and SynQt attached handlers.

    A named type falls through the stock lexer as plain text; here the declared object
    (`ApplicationWindow {`) and the addressed object (`Caller.hasScope(...)`) get
    `Name.Class`, the color `contract Feed` gets in a `.syn` file. The framework accessors
    (`Server`, `Session`, `Router`, `App`, `Caller`, `Client`, from docs/runtime-api.md) get
    a color of their own. An `identifier.chain:` binding is split so `Auth.onLoginFailed:`
    keeps its type; `onClicked:` falls through to the inherited rule.
    """

    name = "SynQt QML"
    aliases = ["synqt-qml"]
    filenames = []
    mimetypes = []

    # JavaScript globals, already built-ins in the inherited lexer. The qualifier rule steps
    # over them (`Math.hypot(...)`).
    javascript_globals = (
        "Array", "ArrayBuffer", "Boolean", "Date", "Error", "EvalError", "Function",
        "Infinity", "Intl", "JSON", "Map", "Math", "NaN", "Number", "Object", "Promise",
        "Proxy", "RangeError", "ReferenceError", "Reflect", "RegExp", "Set", "String",
        "Symbol", "SyntaxError", "TypeError", "URIError", "WeakMap", "WeakSet",
    )

    # The accessors from docs/runtime-api.md, matched as whole words (`SessionSource` stays
    # a type).
    runtime_accessors = ("App", "Caller", "Client", "Router", "Server", "Session")

    tokens = {
        "root": [
            # A module name (`QtQuick.Controls`), left alone by the qualifier rule.
            (r"(import|pragma)([ \t]+)([\w.]+)",
             bygroups(Keyword.Reserved, Whitespace, Name.Other)),
            # An accessor, matched anywhere before the type rules claim it.
            (words(runtime_accessors, prefix=r"\b", suffix=r"\b"), Name.Builtin.Accessor),
            # A type being instantiated: the name before `{` on the same line
            # (`ApplicationWindow {`, `Qt.labs.settings.Settings {`).
            (r"([A-Z]\w*(?:\.[A-Z]\w*)*)([ \t]*)(\{)",
             bygroups(Name.Class, Whitespace, Punctuation)),
            # `Behavior on width { ... }`.
            (r"(Behavior)(\s+)(on)(\s+)(\w+)([ \t]*)(\{)",
             bygroups(Name.Class, Whitespace, Keyword, Whitespace, Name, Whitespace,
                      Punctuation)),
            # An attached signal handler (`Auth.onLoginFailed:`, `Component.onCompleted:`):
            # split the type from the handler.
            (r"([A-Z]\w*)(\.)(on[A-Z]\w*\s*:)", bygroups(Name.Class, Punctuation, Keyword)),
            # A type being addressed: the head of `Caller.hasScope(...)`,
            # `Server.feed.rows`, `Layout.fillWidth:`, `Text.WordWrap`. The rest of the
            # chain stays plain.
            (r"(?!(?:%s)\b)[A-Z]\w*(?=\.)" % "|".join(javascript_globals), Name.Class),
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


class SynqtYamlLexer(YamlLexer):
    """YAML that lexes a connect point ``export:`` block with :class:`SynLexer`.

    Only the block scalar directly under an `export:` key is taken, following the key rather
    than matching text; the rest of the file is plain YAML.
    """

    name = "SynQt YAML"
    aliases = ["synqt-yaml"]
    filenames = []
    mimetypes = []

    def get_tokens_unprocessed(self, text):
        contract = SynLexer()
        inside = False
        for index, token, value in super().get_tokens_unprocessed(text):
            if token is Name.Tag:
                # Any other key ends the block scalar.
                inside = value.strip() == "export"
                yield index, token, value
                continue
            if inside and token in Name.Constant:
                for offset, kind, part in contract.get_tokens_unprocessed(value):
                    yield index + offset, kind, part
                continue
            yield index, token, value
