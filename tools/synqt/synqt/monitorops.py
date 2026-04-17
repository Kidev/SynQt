# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Operator credentials for the monitoring console.

The console has its own identity system, separate from the application's. This module
produces a credential's stored form: name, iteration count, random salt and PBKDF2-SHA256
hash. The password is never stored. There is no `list` or `remove`: the list lives in the
monitor environment (`SYNQT_MONITOR_OPERATORS`), and `add` prints the line to put there.
"""

from __future__ import annotations

import hashlib
import os
import re
from typing import Optional

#: Must match SynQt::OperatorStore::MinimumIterations, which refuses anything weaker.
MINIMUM_ITERATIONS = 600000

#: Must match SynQt::OperatorStore::credentialVariable().
CREDENTIAL_VARIABLE = "SYNQT_MONITOR_OPERATORS"

_NAME = re.compile(r"^[A-Za-z0-9._-]+$")


class MonitorOpsError(Exception):
    """A credential that cannot be minted, surfaced to the CLI without a traceback."""


def mint(name: str, password: str, iterations: int = MINIMUM_ITERATIONS) -> str:
    """The stored form of one operator credential: `name:iterations:saltHex:hashHex`."""
    if not _NAME.match(name or ""):
        raise MonitorOpsError(
            f"'{name}' is not a usable operator name; letters, digits, dot, dash and "
            "underscore only, because it is parsed out of one environment variable")
    if len(password or "") < 12:
        # Refused: this credential is the only gate on the console.
        raise MonitorOpsError(
            "an operator password must be at least 12 characters; this one credential is "
            "the whole gate on a console that shows every request the system has served")
    rounds = max(MINIMUM_ITERATIONS, int(iterations))
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds, 32)
    return f"{name}:{rounds}:{salt.hex()}:{digest.hex()}"


def instructions(entry: str, env_file: Optional[str] = None) -> str:
    """What to do with a freshly minted credential, said once and completely."""
    name = entry.split(":", 1)[0]
    where = env_file or f"the monitor entity's environment ({CREDENTIAL_VARIABLE})"
    return "\n".join([
        f"operator '{name}' created. Add this line to {where}:",
        "",
        f"  {CREDENTIAL_VARIABLE}={entry}",
        "",
        "Several operators go in the same variable, separated by spaces. The password is",
        "not stored anywhere and cannot be recovered; mint a new credential to change it.",
        "This line holds no password and cannot be signed in with, but it is worth as much",
        "as the console it opens, so keep it out of the repository.",
    ])
