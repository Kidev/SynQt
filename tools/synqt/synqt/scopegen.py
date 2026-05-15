# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The scope vocabulary, as a QML enum generated from ``scopes.order``.

:mod:`synqt.appgen` writes ``Scope.qml`` and :mod:`synqt.check` validates a mapping hook
against the same members. A member's value is its index in ``scopes.order`` (its authority
rank), so the edge resolves a hook answer as ``scopeOrder[value]`` with a bounds check.
"""

from posixpath import dirname, join
from typing import Dict, List, Tuple

# The enum name inside ``Scope.qml``. SynQt writes ``Scope.<Member>``; QML also accepts
# ``Scope.Value.<Member>``.
ENUM_NAME = "Value"


def member_name(scope: str) -> str:
    """The QML enum member for one scope name: underscores separate words and the first letter
    is upper case (``power_user`` to ``PowerUser``). Anything else is kept, so an unusable
    name fails validation.
    """
    return "".join(part[:1].upper() + part[1:] for part in str(scope).split("_") if part)


def members(order: List[str]) -> List[Tuple[str, str]]:
    """``(scope, member)`` for each declared scope, in declaration order.

    Raises ``ValueError`` when two scopes map to one member, and for a scope called
    ``value``: the enum is named ``Value``, so ``Scope.Value`` would be ambiguous and QML
    resolves it to the enum.
    """
    seen: Dict[str, str] = {}
    pairs: List[Tuple[str, str]] = []
    for scope in order:
        member = member_name(scope)
        if member == ENUM_NAME:
            raise ValueError(
                f"scope '{scope}' becomes the enum member '{member}', which is also what "
                f"this enum is called, so 'Scope.{member}' would name both; rename the "
                f"scope")
        if not member.isidentifier():
            raise ValueError(
                f"scope '{scope}' does not make a QML enum member ('{member}'); scope names "
                f"are lower case words separated by underscores")
        if member in seen:
            raise ValueError(
                f"scopes '{seen[member]}' and '{scope}' both become the enum member "
                f"'{member}'; rename one, because one member cannot mean two scopes")
        seen[member] = scope
        pairs.append((scope, member))
    return pairs


def render_scope_qml(order: List[str]) -> str:
    """``Scope.qml``: the declared vocabulary as a QML enum, with no ``Unset`` member, so an
    out-of-range hook answer is refused.
    """
    pairs = members(order)
    if not pairs:
        # `enum Value { }` is not QML. Report the empty `scopes.order` here.
        raise ValueError(
            "scopes.order is empty, so there is no vocabulary to generate; declare the "
            "scopes this project's sessions can hold, lowest authority first")
    names = ", ".join(member for _, member in pairs)
    listing = "\n".join(f"//   {index} = {scope}" for index, (scope, _) in enumerate(pairs))
    return f"""// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Generated from scopes.order in synqt.yaml. Do not edit.
//
// Each member's value is its index in scopes.order, which is also its rank under
// scopes.hierarchical:
{listing}
import QtQml

QtObject {{
    enum {ENUM_NAME} {{ {names} }}
}}
"""


def scope_qml_path(hook_relative: str) -> str:
    """Where ``Scope.qml`` goes for a hook at `hook_relative`: beside the mirrored hook under
    ``generated/``, so the hook reaches ``Scope.Admin`` through its own directory with no
    import.
    """
    folder = dirname(hook_relative)
    return join(folder, "Scope.qml") if folder else "Scope.qml"
