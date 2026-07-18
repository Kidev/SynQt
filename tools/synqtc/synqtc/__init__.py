# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""synqtc: the SynQt contract generator.

Parses a ``.syn`` contract file and lowers it to a QtRemoteObjects ``.rep`` file
plus the owner-side Source helper and consumer-side Replica QML registration. The
``.syn`` surface keeps QtRO's safe defaults obvious: ``prop`` becomes a READPUSH
property (never READWRITE), ``model`` exposes only its declared roles, and a
``slot`` with a return type becomes an asynchronous call on the consumer.
"""

from .errors import SynError
from .model import Contract, Field, Model, Param, Prop, Record, Signal, Slot, SynFile
from .parser import parse_file, parse_text

__all__ = [
    "SynError",
    "Contract",
    "Field",
    "Model",
    "Param",
    "Prop",
    "Record",
    "Signal",
    "Slot",
    "SynFile",
    "parse_file",
    "parse_text",
]
