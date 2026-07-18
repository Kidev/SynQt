# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The single error type the generator raises for malformed input."""

from __future__ import annotations


class SynError(Exception):
    """A contract-level error with a source location, reported to the developer.

    The CLI formats it as ``path:line:col: error: message`` and exits non-zero, so
    a malformed contract fails the build clearly rather than emitting broken rep.
    """

    def __init__(self, message: str, *, path: str = "<input>", line: int = 0, col: int = 0) -> None:
        super().__init__(message)
        self.message = message
        self.path = path
        self.line = line
        self.col = col

    def format(self) -> str:
        if self.line > 0:
            return f"{self.path}:{self.line}:{self.col}: error: {self.message}"
        return f"{self.path}: error: {self.message}"
