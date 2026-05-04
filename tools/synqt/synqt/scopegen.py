# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The scope vocabulary, as a QML enum generated from ``scopes.order``.

One module rather than a helper in each caller, because two of them need the same answer:
:mod:`synqt.appgen` writes ``Scope.qml`` and :mod:`synqt.check` validates a mapping hook
against the members it would have written. Two copies of the name mapping would be two
answers to "what is ``power_user`` called", and the one that is wrong is whichever one is
not the file on disk.

A member's value is its index in ``scopes.order``, which is also its authority rank under
``scopes.hierarchical``. That is what removes the round trip. The hook returns the member,
the edge is handed the integer, and resolving it is ``scopeOrder[value]`` with a bounds
check. There is no spelling anywhere in the middle to keep in step.
"""

from posixpath import dirname, join
from typing import Dict, List, Tuple


def member_name(scope: str) -> str:
    """The QML enum member for one scope name.

    QML requires an upper-case first letter on an enum member, and ``scopes.order`` is
    written lower case, so the two cannot be the same string. Underscores separate words
    (``power_user`` to ``PowerUser``). Anything else in the name is kept as it is, so a
    name the mapping cannot make into an identifier fails validation rather than being
    quietly rewritten into something that compiles.
    """
    return "".join(part[:1].upper() + part[1:] for part in str(scope).split("_") if part)


def members(order: List[str]) -> List[Tuple[str, str]]:
    """``(scope, member)`` for each declared scope, in declaration order.

    Raises ``ValueError`` if two scopes map to one member. Merging them silently would give
    two declared scopes one enum value, so a hook asking for one would get the other and
    every check downstream would agree with the wrong answer.
    """
    seen: Dict[str, str] = {}
    pairs: List[Tuple[str, str]] = []
    for scope in order:
        member = member_name(scope)
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
    """``Scope.qml``: the declared vocabulary as a QML enum.

    Without an ``Unset`` member. Every member is a scope the project declared,
    which is what makes an out-of-range answer from a mapping hook a refusal rather than a
    fallback.
    """
    pairs = members(order)
    if not pairs:
        # `enum Value { }` is not QML, and the failure a project would get for it comes out
        # of qmlcachegen naming a generated file the author never wrote. Say it here, where
        # the thing to fix (an empty `scopes.order` on a project that signs people in) is
        # still the thing being talked about.
        raise ValueError(
            "scopes.order is empty, so there is no vocabulary to generate; declare the "
            "scopes this project's sessions can hold, lowest authority first")
    names = ", ".join(member for _, member in pairs)
    listing = "\n".join(f"//   {index} = {scope}" for index, (scope, _) in enumerate(pairs))
    return f"""// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Generated from scopes.order in synqt.yaml. Do not edit. Edit the project instead.
//
// A member's value is its index in scopes.order, which is also its authority rank under
// scopes.hierarchical:
{listing}
import QtQml

QtObject {{
    enum Value {{ {names} }}
}}
"""


def scope_qml_path(hook_relative: str) -> str:
    """Where ``Scope.qml`` goes for a project whose mapping hook is at `hook_relative`.

    Beside the hook, in the same mirrored directory under ``generated/``, and for a reason
    worth keeping written down: a QML component resolves an unqualified type name against
    its own directory first, so a hook that sits next to this file can write
    ``Scope.Value.Admin`` with no import at all. Anywhere else and the hook would need an
    import path, a qmldir and a module URI to reach one enum.
    """
    folder = dirname(hook_relative)
    return join(folder, "Scope.qml") if folder else "Scope.qml"
