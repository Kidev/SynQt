# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""No documentation page publishes a list item as literal `- ` text.

A left-margin list item is absorbed into an open block above it, and neither `mkdocs build
--strict` nor a link checker notices. `test_the_shapes_this_was_measured_against` records
what the renderer does with each shape.
"""

import re
import unittest
from pathlib import Path

DOCS = Path(__file__).resolve().parents[3] / "docs"

#: A left-margin list item. Indented items belong to sub-lists.
_ITEM = re.compile(r"^[-*+] \S")


def swallowed_items(text):
    """Every left-margin list item the block above absorbs, as (line number, text).

    After a blank line an item starts a list. Otherwise it is absorbed when the line above
    is unindented prose, or an indented line of an item already broken by a blank line (a
    loose item's open paragraph). After an ordinary wrapped line it starts correctly.
    """
    found = []
    fenced = False
    blank_since_marker = False
    previous = ""
    for number, line in enumerate(text.split("\n"), start=1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            previous = line
            continue
        if fenced:
            previous = line
            continue
        if _ITEM.match(line) and number > 1:
            if previous.strip() and not _ITEM.match(previous):
                indented = previous.startswith((" ", "\t"))
                structural = previous.startswith((">", "|", "<", "#"))
                if not structural and (not indented or blank_since_marker):
                    found.append((number, line[:60]))
        if _ITEM.match(line) or (line.strip() and not line.startswith((" ", "\t"))):
            blank_since_marker = False
        elif not line.strip():
            blank_since_marker = True
        previous = line
    return found


class DocsMarkdownTest(unittest.TestCase):
    def test_no_list_item_is_swallowed_by_the_block_above_it(self):
        offenders = {}
        for page in sorted(DOCS.glob("*.md")):
            found = swallowed_items(page.read_text(encoding="utf-8"))
            if found:
                offenders[page.name] = found
        self.assertFalse(
            offenders,
            "these list items publish as literal text; put a blank line above each:\n"
            + "\n".join(f"  {name}:{number}: {text}"
                        for name, items in offenders.items()
                        for number, text in items))

    def test_the_shapes_this_was_measured_against(self):
        """What the renderer does with each shape."""
        broken = [
            "- First item\n\n  A second paragraph.\n- Second item\n",
            "- First item\n\n    A second paragraph.\n- Second item\n",
            "A paragraph.\n- An item\n",
        ]
        fine = [
            "- First item that wraps\n  onto a second line\n- Second item\n",
            "- First item\n\n    A second paragraph.\n\n- Second item\n",
            "- First item\n\n  A second paragraph.\n\n- Second item\n",
            "```\nA line.\n- not a list\n```\n",
        ]
        for source in broken:
            self.assertTrue(swallowed_items(source), f"missed: {source!r}")
        for source in fine:
            self.assertFalse(swallowed_items(source), f"false alarm: {source!r}")


if __name__ == "__main__":
    unittest.main()
