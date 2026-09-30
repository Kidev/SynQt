# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Write the provider a tutorial page builds, from the page itself.

The page says its C++ pieces, in order from the first one that opens with an `#include`,
make the whole file. This joins exactly those pieces and nothing else, so the file that
compiles is the one a reader assembles.

    python3 extract.py <page.md> <out.cpp>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_FENCE = re.compile(r"^```cpp\n(.*?)^```", re.MULTILINE | re.DOTALL)


def pieces(page: str) -> list:
    fences = [match.group(1) for match in _FENCE.finditer(page)]
    first = next((index for index, body in enumerate(fences)
                  if body.lstrip().startswith("#include")), None)
    if first is None:
        raise SystemExit("the page has no C++ piece that opens with an #include")
    return fences[first:]


def main(argv: list) -> int:
    page, out = Path(argv[1]), Path(argv[2])
    body = "\n".join(pieces(page.read_text(encoding="utf-8")))
    out.write_text(f"// Extracted from {page.name}; edit the page, not this file.\n\n{body}",
                   encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
