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
    scope: List[str] = field(default_factory=list)


@dataclass
class Role:
    """``<type> <name>`` inside a model's role list. One field of a published row.

    Typed like every other declaration, so the owner-side boundary can check a row
    before it serializes. ``var`` is the escape hatch for a role that does
    carry anything.
    """

    type: str
    name: str
    line: int = 0
    col: int = 0


@dataclass
class Model:
    """``model <name>(<roles...>)``, a live list. Only listed roles cross."""

    name: str
    roles: List[Role]
    line: int = 0
    col: int = 0
    scope: List[str] = field(default_factory=list)


@dataclass
class Signal:
    """``signal <name>(<params>)``; an owner-to-consumer event."""

    name: str
    params: List[Param]
    line: int = 0
    col: int = 0
    scope: List[str] = field(default_factory=list)


@dataclass
class Slot:
    """``slot [<return>] <name>(<params>)``. A consumer-to-owner request.

    ``return_type`` is ``None`` for a fire-and-forget slot (lowered to a void
    slot). A present return type makes it an asynchronous call on the consumer.
    """

    name: str
    params: List[Param]
    return_type: Union[str, None] = None
    line: int = 0
    col: int = 0
    scope: List[str] = field(default_factory=list)


Member = Union[Prop, Model, Signal, Slot]

#: Every member carries a ``scope``: the scopes whose holder may reach it, any one of them
#: being enough, and empty meaning everyone the point is hosted for. It is written as a
#: ``<admin>`` prefix, and a member that wrote none inherits the connect point's ``scope:``,
#: which the CLI has already filled in by the time a ``.syn`` reaches the compiler.
#:
#: The gate is on the flow, not on the shape. QtRO matches a Replica to a Source by
#: signature, so an out-of-scope member is still declared and still acquired. What does not
#: happen is that anything crosses. See :func:`synqtc.emit` for the mirror that enforces it.


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
    """One parsed ``.syn`` file: its records and contracts, in source order.

    ``forwards_session`` is not written in the file. It is how the build asked for the file
    to be compiled. A connect point that a service consumes is reached over the mesh, where
    the calling entity may be answering someone further up the chain, so every slot on it
    carries the session that entity is acting for. A point only a browser consumes carries
    nothing extra, and there is then no field a browser could fill.
    """

    stem: str
    records: List[Record] = field(default_factory=list)
    contracts: List[Contract] = field(default_factory=list)
    forwards_session: bool = False

    @property
    def record_names(self) -> List[str]:
        return [record.name for record in self.records]
