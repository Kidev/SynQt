# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The parsed contract AST.

One ``.syn`` file becomes a :class:`SynFile` holding its records and contracts in
source order. Every node keeps the source line it started on so lowering can point
back to it if a later validation fails.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Union


@dataclass
class Param:
    """A typed, named parameter of a slot, signal, or record field."""

    type: str
    name: str
    line: int = 0
    col: int = 0


# A record field is spelled exactly like a parameter.
Field = Param


@dataclass
class Prop:
    """``prop <type> <name>``; an owner-held value pushed to consumers."""

    type: str
    name: str
    line: int = 0
    col: int = 0


@dataclass
class Model:
    """``model <name>(<roles...>)``; a live list; only listed roles cross."""

    name: str
    roles: List[str]
    line: int = 0
    col: int = 0


@dataclass
class Signal:
    """``signal <name>(<params>)``; an owner-to-consumer event."""

    name: str
    params: List[Param]
    line: int = 0
    col: int = 0


@dataclass
class Slot:
    """``slot [<return>] <name>(<params>)``; a consumer-to-owner request.

    ``return_type`` is ``None`` for a fire-and-forget slot (lowered to a void
    slot); a present return type makes it an asynchronous call on the consumer.
    """

    name: str
    params: List[Param]
    return_type: Union[str, None] = None
    line: int = 0
    col: int = 0


Member = Union[Prop, Model, Signal, Slot]


@dataclass
class Contract:
    """``contract <name> { <members> }``; the API of one connect point."""

    name: str
    members: List[Member] = field(default_factory=list)
    line: int = 0
    col: int = 0

    @property
    def props(self) -> List[Prop]:
        return [m for m in self.members if isinstance(m, Prop)]

    @property
    def models(self) -> List[Model]:
        return [m for m in self.members if isinstance(m, Model)]

    @property
    def signals(self) -> List[Signal]:
        return [m for m in self.members if isinstance(m, Signal)]

    @property
    def slots(self) -> List[Slot]:
        return [m for m in self.members if isinstance(m, Slot)]


@dataclass
class Record:
    """``record <name>(<fields>)``; a plain data record, lowered to a POD."""

    name: str
    fields: List[Field] = field(default_factory=list)
    line: int = 0
    col: int = 0


@dataclass
class SynFile:
    """One parsed ``.syn`` file: its records and contracts, in source order."""

    stem: str
    records: List[Record] = field(default_factory=list)
    contracts: List[Contract] = field(default_factory=list)

    @property
    def record_names(self) -> List[str]:
        return [record.name for record in self.records]
