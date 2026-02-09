# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Operator credentials for the monitoring console.

The monitor holds every entity's record, so a single sign-in there reveals what the whole
system has been doing. It therefore has an identity system of its own, separate from the
application's, because an operator is not a user of the application, and a project's own
login provider is often the thing an operator is signing in to investigate.

What this module produces is the *stored form* of a credential: a name, an iteration count,
a random salt, and a PBKDF2-SHA256 hash. The password itself is never written anywhere, and
the stored form is not a secret in the sense a password is. It cannot be replayed, only
brute-forced, which is what the iteration count is for.

There is no `list` and no `remove`. The operator list lives in the monitor
entity's environment (`SYNQT_MONITOR_OPERATORS`), like every other credential in SynQt, and
a CLI that pretended to own that list would be a CLI that edits whatever file the deployment
happens to keep it in. `add` prints a line. Where it goes is the deployment's business.
"""

from __future__ import annotations

import hashlib
import os
import re
from typing import Optional

#: Must match SynQt::OperatorStore::MinimumIterations. The store refuses anything weaker,
#: so a mismatch here would produce credentials the monitor will not load.
MINIMUM_ITERATIONS = 600000

#: Must match SynQt::OperatorStore::credentialVariable().
CREDENTIAL_VARIABLE = "SYNQT_MONITOR_OPERATORS"

_NAME = re.compile(r"^[A-Za-z0-9._-]+$")


class MonitorOpsError(Exception):
    """A credential that cannot be minted, surfaced to the CLI without a traceback."""


def mint(name: str, password: str, iterations: int = MINIMUM_ITERATIONS) -> str:
    """The stored form of one operator credential: `name:iterations:saltHex:hashHex`.

    The password is used and dropped. What comes back is what goes in the environment.
    """
    if not _NAME.match(name or ""):
        raise MonitorOpsError(
            f"'{name}' is not a usable operator name; letters, digits, dot, dash and "
            "underscore only, because it is parsed out of one environment variable")
    if len(password or "") < 12:
        # Refused, not warned about. This credential is the whole gate on a console that
        # shows every request the system has served.
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
        "This line is not a password and cannot be signed in with, but it is worth as much",
        "as the console it opens, so keep it out of the repository.",
    ])
