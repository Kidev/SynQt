# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The contract type vocabulary and its lowering to C++/rep types.

A type is a name and, for the ones that can be bounded, a size in brackets:
``string[64]`` is at most 64 characters, ``int16`` is a 16-bit integer. The size is
part of the contract, not a comment about it: it decides how wide the value is on the
wire, and the generated owner-side boundary refuses a value that does not fit rather
than truncating it into something that looks fine.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Optional, Tuple

from .errors import SynError

# The built-in scalar types a contract may use, mapped to their C++ spelling.
#
# The sized integers are here for the same reason a schema has them: a score that cannot
# exceed a few thousand does not need eight bytes on every snapshot, and writing the width
# down is also the only way a reader learns what the value is allowed to be. `int` is
# int32 under another name, kept because most values are just numbers.
BUILTIN_TYPES = {
    "int": "int",
    "int8": "qint8",
    "int16": "qint16",
    "int32": "int",
    "int64": "qint64",
    "uint8": "quint8",
    "uint16": "quint16",
    "uint32": "quint32",
    "uint64": "quint64",
    "string": "QString",
    "bool": "bool",
    "real": "double",
    "float": "float",
    "double": "double",
    "var": "QVariant",
}

#: The inclusive range of each sized integer, for the boundary check the generator emits.
#: Written out rather than computed so the generated code reads as the numbers a reviewer
#: can check against the contract.
INT_RANGES = {
    "int": (-2147483648, 2147483647),
    "int8": (-128, 127),
    "int16": (-32768, 32767),
    "int32": (-2147483648, 2147483647),
    "int64": (-9223372036854775808, 9223372036854775807),
    "uint8": (0, 255),
    "uint16": (0, 65535),
    "uint32": (0, 4294967295),
    "uint64": (0, 18446744073709551615),
}

#: Types that accept a bracketed size, and what the size means for each.
SIZED_TYPES = {"string": "characters"}

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
    """Split a written type into its base name and its bound.

    Only the shape is checked here (``name`` or ``name[123]``). Whether the name resolves
    and whether it may carry a bound at all is :func:`cpp_type`'s job, because that is
    where the records declared alongside it are known.
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


def int_range(spelling: str) -> Optional[Tuple[int, int]]:
    """The inclusive range of a written integer type, or None for anything else."""
    return INT_RANGES.get(base_of(spelling))


def cpp_type(
    syn_type: str,
    record_names: Iterable[str],
    *,
    path: str,
    line: int,
    col: int,
) -> str:
    """Lower a written type to its C++/rep spelling.

    A type is valid if it is a built-in scalar or a record declared in the same contract
    (records lower to their POD type by name). A bound is valid only on a type that has
    something to bound; anything else is a clear error.
    """
    reference = parse_type(syn_type, path=path, line=line, col=col)
    known = sorted(BUILTIN_TYPES) + sorted(record_names)
    if reference.base not in BUILTIN_TYPES and reference.base not in record_names:
        raise SynError(
            f"unknown type '{reference.base}' (known types: {', '.join(known)})",
            path=path,
            line=line,
            col=col,
        )
    if reference.size is not None and reference.base not in SIZED_TYPES:
        bounded = ", ".join(sorted(SIZED_TYPES))
        raise SynError(
            f"'{reference.base}' takes no bound in brackets; the types that do are "
            f"{bounded} (an integer says its width in its name: int16, uint8, ...)",
            path=path,
            line=line,
            col=col,
        )
    if reference.base in BUILTIN_TYPES:
        return BUILTIN_TYPES[reference.base]
    return reference.base
