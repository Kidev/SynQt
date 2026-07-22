# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The version the release workflow stamps before freezing a standalone binary.

A frozen binary has no installed distribution to query, so this file is the fallback
that keeps `synqt --version` honest there. Leave it at the development placeholder in
git; the release job rewrites it.
"""

__version__ = "0.0.0.dev0"
