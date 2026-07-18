# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The ``.syn`` type vocabulary and its lowering to C++/rep types."""

from __future__ import annotations

from typing import Iterable

from .errors import SynError

# The built-in scalar types a contract may use, mapped to their C++ spelling.
BUILTIN_TYPES = {
    "int": "int",
    "string": "QString",
    "bool": "bool",
    "real": "double",
    "float": "float",
    "double": "double",
    "var": "QVariant",
}


def cpp_type(
    syn_type: str,
    record_names: Iterable[str],
    *,
    path: str,
    line: int,
    col: int,
) -> str:
    """Lower a ``.syn`` type to its C++/rep spelling.

    A type is valid if it is a built-in scalar or a record declared in the same
    file (records lower to their POD type by name). Anything else is a clear error.
    """
    if syn_type in BUILTIN_TYPES:
        return BUILTIN_TYPES[syn_type]
    if syn_type in record_names:
        return syn_type
    known = ", ".join(sorted(BUILTIN_TYPES) + sorted(record_names))
    raise SynError(
        f"unknown type '{syn_type}' (known types: {known})",
        path=path,
        line=line,
        col=col,
    )
