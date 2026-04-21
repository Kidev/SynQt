# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A member docs/runtime-api.md writes with parentheses is a callable in the header, and one
written without is not.

In QML a method used without parentheses is a function object, not a value. Checked against
the headers of the helpers whose surface is entirely C++; `Server` and the connect point
accessors come from the application's `export:` and are excluded. Signals count as calls.
"""

import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
REFERENCE = REPO_ROOT / "docs" / "runtime-api.md"

#: The accessor names whose surface is entirely in one header.
HELPERS = {
    "Db": "src/providers/db.h",
    "Docs": "src/providers/docs.h",
    "Cache": "src/providers/cache.h",
    "Jobs": "src/providers/jobs.h",
    "Log": "src/service/log.h",
    "Http": "src/providers/http.h",
    "Api": "src/gateway/api.h",
}

#: The name is the last identifier before the open parenthesis.
_INVOKABLE = re.compile(r"Q_INVOKABLE\s+[^(;]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(")
_PROPERTY = re.compile(r"Q_PROPERTY\s*\(\s*[^\s]+\s+([A-Za-z_][A-Za-z0-9_]*)\s")
#: Signals are read from the `signals:` block.
_SIGNAL_BLOCK = re.compile(r"^\s*signals:\s*$(.*?)(?=^\s*(?:public|private|protected)\b|\Z)",
                           re.MULTILINE | re.DOTALL)
_DECLARATION = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")

#: A row of one of the reference's member tables. `| \`Db.query(sql)\` | list | ... |`.
_ROW = re.compile(r"^\|\s*`([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)(\(?)")


def header_surface(relative_path):
    """The callable names and the property names one helper header declares."""
    text = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
    callable_names = set(_INVOKABLE.findall(text))
    for block in _SIGNAL_BLOCK.findall(text):
        callable_names.update(_DECLARATION.findall(block))
    return callable_names, set(_PROPERTY.findall(text))


def documented_members():
    """Every `<Helper>.<member>` row of the reference, and whether it is written as a call."""
    found = []
    for line in REFERENCE.read_text(encoding="utf-8").splitlines():
        match = _ROW.match(line)
        if match and match.group(1) in HELPERS:
            found.append((match.group(1), match.group(2), bool(match.group(3))))
    return found


def test_the_reference_documents_these_helpers_at_all():
    # The tables must not be empty.
    documented = documented_members()
    assert len(documented) >= 20
    assert {helper for helper, _, _ in documented} == set(HELPERS)


@pytest.mark.parametrize("helper,member,as_call", documented_members(),
                         ids=lambda value: str(value))
def test_a_documented_member_is_spelled_the_way_the_header_declares_it(helper, member, as_call):
    callable_names, property_names = header_surface(HELPERS[helper])
    known = callable_names | property_names
    assert member in known, (
        f"{helper}.{member} is in {REFERENCE.name} and {HELPERS[helper]} declares no such "
        "member")
    if as_call:
        assert member in callable_names, (
            f"{helper}.{member}() is documented as a call and {HELPERS[helper]} declares it "
            "as a property; QML would call a value")
    else:
        assert member in property_names, (
            f"{helper}.{member} is documented as a value and {HELPERS[helper]} declares it "
            "as a method; QML reads that as a function object, not as the value")
