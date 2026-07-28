# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Put `benchmarks/` on the path so the tests can import the checker.

`baselines.py` is a script rather than an installed package: it has no dependencies
beyond the standard library and is run straight out of the tree by CI and by hand, so
there is nothing to `pip install -e`.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
