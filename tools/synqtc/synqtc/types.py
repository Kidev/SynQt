# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The contract type vocabulary and its lowering to C++/rep types.

The vocabulary is the built-in QML value types: `bool`, `date`, `double`, `int`, `list`,
`real`, `string`, `url`, `var`, `variant`. Four take a size in brackets: `string[64]` is at
most 64 characters, `list[100]` at most 100 elements, `var[4096]` at most 4096 serialized
bytes. The generated boundary refuses a value that does not fit.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Optional

from .errors import SynError

#: The built-in QML value types, mapped to their C++ spelling
#: (https://doc.qt.io/qt-6/qtqml-typesystem-valuetypes.html). `list` is a list of `var`; a
#: typed list of rows is a `model`.
BUILTIN_TYPES = {
    "bool": "bool",
    "date": "QDateTime",
    "double": "double",
    "int": "int",
    "list": "QVariantList",
    "real": "double",
    "string": "QString",
    "url": "QUrl",
    "var": "QVariant",
    "variant": "QVariant",
}

#: The types that accept a bracketed size, and what the size counts.
SIZED_TYPES = {
    "string": "characters",
    "url": "characters",
    "list": "elements",
    "var": "bytes",
    "variant": "bytes",
}

#: How the generated boundary measures a value of each bounded type, given an expression of
#: its C++ type. `var` is measured by its serialized size.
MEASURES = {
    "string": lambda expr: f"{expr}.size()",
    "url": lambda expr: f"{expr}.toString().size()",
    "list": lambda expr: f"{expr}.size()",
    "var": lambda expr: f"synqtVariantBytes({expr})",
    "variant": lambda expr: f"synqtVariantBytes({expr})",
}

#: The types a value crosses as-is, with no conversion. `var` and its older spelling.
GENERIC_TYPES = {"var", "variant"}

#: Spelled by QML but not declarable, and each with a better answer in this grammar.
NOT_A_DECLARATION = {
    "void": "a slot with no return type is already void, so write 'slot {name}(...)'",
    "enumeration": "an enumeration is not a value a contract can carry; send the "
                   "'int' or the 'string' it stands for",
}

_TYPE_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\[([0-9]+)\])?$")


@dataclass(frozen=True)
class TypeRef:
    """One written type: its base name and the bound it was written with, if any."""

    base: str
    size: Optional[int] = None

    def __str__(self) -> str:
        return self.base if self.size is None else f"{self.base}[{self.size}]"


def parse_type(
    spelling: str,
    *,
    path: str,
    line: int,
    col: int,
) -> TypeRef:
    """Split a written type into its base name and bound. Only the shape is checked;
    :func:`cpp_type` validates the name and whether it may carry a bound.
    """
    match = _TYPE_RE.match(spelling)
    if not match:
        raise SynError(f"'{spelling}' is not a type", path=path, line=line, col=col)
    base, size = match.group(1), match.group(2)
    if size is None:
        return TypeRef(base=base)
    bound = int(size)
    if bound <= 0:
        raise SynError(
            f"'{spelling}' bounds the value at {bound}; a bound is a positive number of "
            f"{SIZED_TYPES.get(base, 'units')}",
            path=path, line=line, col=col,
        )
    return TypeRef(base=base, size=bound)


def base_of(spelling: str) -> str:
    """The base name of a written type, ignoring any bound. Never raises."""
    match = _TYPE_RE.match(spelling)
    return match.group(1) if match else spelling


def bound_of(spelling: str) -> Optional[int]:
    """The bound a written type carries, or None. Never raises."""
    match = _TYPE_RE.match(spelling)
    if not match or match.group(2) is None:
        return None
    return int(match.group(2))


def is_generic(spelling: str) -> bool:
    """Does this type carry whatever it is handed, with no conversion?"""
    return base_of(spelling) in GENERIC_TYPES


def measure(spelling: str, expr: str) -> Optional[str]:
    """How to measure `expr` against this type's bound, or None if it has none."""
    if bound_of(spelling) is None:
        return None
    sizer = MEASURES.get(base_of(spelling))
    return None if sizer is None else sizer(expr)


def unit_of(spelling: str) -> str:
    """What a bound on this type counts: characters, elements, or bytes."""
    return SIZED_TYPES.get(base_of(spelling), "units")


def cpp_type(
    syn_type: str,
    record_names: Iterable[str],
    *,
    path: str,
    line: int,
    col: int,
) -> str:
    """Lower a written type to its C++/rep spelling: a built-in QML value type, or a record
    declared in the same file (lowered to its POD). A bound only on a boundable type.
    """
    reference = parse_type(syn_type, path=path, line=line, col=col)
    known = sorted(BUILTIN_TYPES) + sorted(record_names)
    if reference.base in NOT_A_DECLARATION and reference.base not in record_names:
        advice = NOT_A_DECLARATION[reference.base].replace("{name}", "<name>")
        raise SynError(
            f"'{reference.base}' is not a type a contract declares; {advice}",
            path=path,
            line=line,
            col=col,
        )
    if reference.base not in BUILTIN_TYPES and reference.base not in record_names:
        raise SynError(
            f"unknown type '{reference.base}' (known types: {', '.join(known)})",
            path=path,
            line=line,
            col=col,
        )
    if reference.size is not None and reference.base not in SIZED_TYPES:
        bounded = ", ".join(f"{name}[n] ({unit})" for name, unit in sorted(SIZED_TYPES.items()))
        raise SynError(
            f"'{reference.base}' takes no bound in brackets; the types that do are "
            f"{bounded}",
            path=path,
            line=line,
            col=col,
        )
    if reference.base in BUILTIN_TYPES:
        return BUILTIN_TYPES[reference.base]
    return reference.base
