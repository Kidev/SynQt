# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Change one thing in synqt.yaml and leave the rest of the file exactly as it was.

Loading a file with `yaml.safe_load`, editing the object and dumping it back is the
obvious way to do this and the wrong one: the dump is a new document. Comments are gone,
the key order is whatever the dumper felt like, blank lines that grouped related entries
have closed up, and a hand-written `consumers: [client]` has become two lines. The author
wrote that file; a scaffold command, and later the visual editor, have no business
reformatting it to make one edit.

So every function here works on the text. It locates the lines the edit belongs to, splices
new lines in, and returns the rest byte for byte. What it cannot locate textually it
refuses, loudly, rather than falling back to a whole-document rewrite: a flow-style list is
legal YAML and not a shape this project writes, so meeting one means the file is not what
the caller thinks it is.

Nothing here touches the disk. Every function takes the file text and returns new text.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Tuple

import yaml

#: Wide enough that safe_dump never wraps a value onto a second line. A wrapped value is
#: still valid YAML and still a formatting change nobody asked for.
_WIDE = 1 << 20

_KEY = re.compile(r"^(?P<indent> *)(?P<key>[A-Za-z_][A-Za-z0-9_.-]*) *:(?P<rest>.*)$")


class YamlEditError(Exception):
    """An edit that cannot be made textually, surfaced to the caller rather than guessed."""


class _Block:
    """A mapping key and the lines beneath it.

    `start`/`end` bound the child lines, `indent` is the column the children sit at (-1
    when there are none yet), and `inline` is whatever followed the colon on the key line.
    """

    def __init__(self, key_line: int, key_indent: int, inline: str,
                 start: int, end: int, indent: int) -> None:
        self.key_line = key_line
        self.key_indent = key_indent
        self.inline = inline
        self.start = start
        self.end = end
        self.indent = indent

    @property
    def child_indent(self) -> int:
        """Where a new child goes: beside its siblings, or one step in if it is the first."""
        return self.indent if self.indent >= 0 else self.key_indent + 2


def append_item(text: str, list_path: str, item: Dict[str, Any], *,
                comment: str = "") -> str:
    """`text` with `item` added to the block list at `list_path`, everything else intact.

    The list is created when the file does not have it yet. `comment` is written on its own
    line above the item, so a generated entry can say where it came from.
    """
    lines = _split(text)
    parent, key, key_line = _locate(lines, list_path)
    if key_line is None:
        indent = parent.child_indent
        rendered = ([" " * indent + f"# {comment}"] if comment else [])
        rendered += [" " * indent + key + ":"]
        rendered += _render_item(item, indent + 2)
        at = _trimmed_end(lines, parent.start, parent.end)
        return _join(lines[:at] + rendered + lines[at:])

    block = _block_at(lines, key_line)
    items = _list_items(lines, block, list_path)
    indent = items[0].indent if items else block.child_indent
    rendered = ([" " * indent + f"# {comment}"] if comment else [])
    rendered += _render_item(item, indent)
    if not items and _is_empty_flow(block.inline):
        # "connect_points: []", which is what remove_item leaves behind. Reopen it.
        lines = list(lines)
        lines[key_line] = lines[key_line][:lines[key_line].index(":") + 1]
        return _join(lines[:key_line + 1] + rendered + lines[key_line + 1:])
    at = _trimmed_end(lines, block.start, block.end)
    return _join(lines[:at] + rendered + lines[at:])


def patch_item(text: str, list_path: str, name: str, fields: Dict[str, Any]) -> str:
    """`text` with `fields` set on the item named `name`, and nothing else touched.

    A field the item does not have yet is added after the ones it does.
    """
    lines = _split(text)
    _, items = _list_of(lines, list_path)
    item = _item_named(items, name, list_path)

    fragment = _deindented(lines[item.start:item.end], item.indent)
    for key, value in fields.items():
        fragment = _put_key(fragment, _root_block(fragment), key, value)
    return _join(lines[:item.start] + _reindented(fragment, item.indent)
                 + lines[item.end:])


def remove_item(text: str, list_path: str, name: str) -> str:
    """`text` with the item named `name` gone, along with the comment written above it.

    Emptying the list writes `[]` rather than leaving the key bare, because a bare key
    reads back as null and every caller here expects a list.
    """
    lines = _split(text)
    block, items = _list_of(lines, list_path)
    item = _item_named(items, name, list_path)
    start, end = item.start, item.end
    list_end = _trimmed_end(lines, block.start, block.end)

    while start > block.start and _is_comment(lines[start - 1]):
        start -= 1
    while end < list_end and not lines[end].strip():
        end += 1

    lines = lines[:start] + lines[end:]
    if len(items) == 1:
        key_line = block.key_line
        lines[key_line] = lines[key_line][:lines[key_line].index(":") + 1] + " []"
    return _join(lines)


def set_scalar(text: str, path: str, value: Any) -> str:
    """`text` with `path` set to `value`, adding the key when the parent does not have it.

    `value` is anything YAML can write. A mapping arrives as an indented block, which is
    how a whole section (`identity`) gets written in one call.
    """
    lines = _split(text)
    parent, key, _ = _locate(lines, path)
    return _join(_put_key(lines, parent, key, value))


# Reading the text.

def _split(text: str) -> List[str]:
    return text.split("\n")


def _join(lines: List[str]) -> str:
    return "\n".join(lines)


def _indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _is_comment(line: str) -> bool:
    return line.lstrip().startswith("#")


def _is_skippable(line: str) -> bool:
    """Blank lines and comments belong to no block in particular."""
    return not line.strip() or _is_comment(line)


def _is_empty_flow(inline: str) -> bool:
    return inline.strip() in ("[]", "{}")


def _end_of_block(lines: List[str], start: int, parent_indent: int) -> int:
    """One past the last line indented deeper than `parent_indent`."""
    index = start
    while index < len(lines):
        line = lines[index]
        if not _is_skippable(line) and _indent_of(line) <= parent_indent:
            break
        index += 1
    return index


def _trimmed_end(lines: List[str], start: int, end: int) -> int:
    """`end` walked back over the blanks and comments that introduce whatever comes next."""
    while end > start and _is_skippable(lines[end - 1]):
        end -= 1
    return end


def _child_indent(lines: List[str], start: int, end: int) -> int:
    for index in range(start, end):
        if not _is_skippable(lines[index]):
            return _indent_of(lines[index])
    return -1


def _root_block(lines: List[str]) -> _Block:
    end = len(lines)
    return _Block(-1, -2, "", 0, end, _child_indent(lines, 0, end))


def _block_at(lines: List[str], key_line: int) -> _Block:
    match = _KEY.match(lines[key_line])
    assert match is not None
    key_indent = len(match.group("indent"))
    start = key_line + 1
    end = _end_of_block(lines, start, key_indent)
    return _Block(key_line, key_indent, match.group("rest"), start, end,
                  _child_indent(lines, start, end))


def _find_key(lines: List[str], block: _Block, key: str) -> Optional[int]:
    if block.indent < 0:
        return None
    for index in range(block.start, block.end):
        line = lines[index]
        if _is_skippable(line) or _indent_of(line) != block.indent:
            continue
        match = _KEY.match(line)
        if match is not None and match.group("key") == key:
            return index
    return None


def _locate(lines: List[str], path: str) -> Tuple[_Block, str, Optional[int]]:
    """The block the last path segment lives in, its key, and its line if it is there."""
    segments = path.split(".")
    block = _root_block(lines)
    for segment in segments[:-1]:
        key_line = _find_key(lines, block, segment)
        if key_line is None:
            raise YamlEditError(f"'{path}': there is no '{segment}' section to edit")
        block = _block_at(lines, key_line)
        if block.inline.strip() and not _is_empty_flow(block.inline):
            raise YamlEditError(f"'{path}': '{segment}' is a value, not a section")
    return block, segments[-1], _find_key(lines, block, segments[-1])


def _list_of(lines: List[str], list_path: str) -> Tuple[_Block, List["_Item"]]:
    parent, key, key_line = _locate(lines, list_path)
    if key_line is None:
        raise YamlEditError(f"'{list_path}': there is no such list in this file")
    block = _block_at(lines, key_line)
    return block, _list_items(lines, block, list_path)


class _Item:
    """One `- name: ...` entry: the lines it occupies and what it parses to."""

    def __init__(self, start: int, end: int, indent: int, value: Dict[str, Any]) -> None:
        self.start = start
        self.end = end
        self.indent = indent
        self.value = value


def _list_items(lines: List[str], block: _Block, list_path: str) -> List[_Item]:
    """Every item in the list. Refuses any shape a text edit cannot reach."""
    if block.inline.strip() and not _is_empty_flow(block.inline):
        raise YamlEditError(
            f"'{list_path}' is written on one line; rewrite it as a block list "
            f"(one '- name: ...' per line) before editing it here")
    end = _trimmed_end(lines, block.start, block.end)
    starts: List[int] = []
    indent = -1
    for index in range(block.start, end):
        line = lines[index]
        if _is_skippable(line):
            continue
        if indent < 0:
            if not line.lstrip().startswith("- "):
                raise YamlEditError(
                    f"'{list_path}' is not a list; it holds '{line.strip()}'")
            indent = _indent_of(line)
        if _indent_of(line) == indent and line.lstrip().startswith("- "):
            starts.append(index)

    items: List[_Item] = []
    for position, start in enumerate(starts):
        stop = starts[position + 1] if position + 1 < len(starts) else end
        stop = _trimmed_end(lines, start, stop)
        value = _item_value(lines, start, stop, indent)
        if not isinstance(value, dict):
            raise YamlEditError(
                f"'{list_path}' holds plain values; these edits address items by name")
        items.append(_Item(start, stop, indent, value))
    return items


def _item_named(items: List[_Item], name: str, list_path: str) -> _Item:
    for item in items:
        if item.value.get("name") == name:
            return item
    raise YamlEditError(f"'{list_path}' has no item named '{name}'")


def _item_value(lines: List[str], start: int, end: int, indent: int) -> Any:
    return yaml.safe_load(_join(_deindented(lines[start:end], indent)))


def _deindented(item_lines: List[str], indent: int) -> List[str]:
    """The item at column zero, its dash turned into the two spaces it stands for."""
    out: List[str] = []
    for line in item_lines:
        body = line[indent:] if len(line) > indent else line.strip()
        out.append("  " + body[2:] if body.startswith("- ") else body)
    return out


def _reindented(item_lines: List[str], indent: int) -> List[str]:
    out: List[str] = []
    for position, line in enumerate(item_lines):
        if position == 0:
            out.append(" " * indent + "- " + line[2:])
        elif line.strip():
            out.append(" " * indent + line)
        else:
            out.append(line)
    return out


# Writing the text.

def _is_scalar(value: Any) -> bool:
    return value is None or isinstance(value, (str, int, float, bool))


def _scalar_text(value: Any) -> str:
    """One scalar written the way YAML needs it, quoting and all.

    Dumped as the value of a throwaway key rather than on its own, because a bare scalar
    document picks up an explicit "..." end marker that would have to be stripped back off.
    """
    text = yaml.safe_dump({"v": value}, default_flow_style=False, width=_WIDE,
                          allow_unicode=True)
    return text.rstrip("\n")[len("v: "):]


def _render_field(key: str, value: Any) -> List[str]:
    """`key: value` as YAML lines at column zero, in the style the project writes.

    Written here rather than handed to `safe_dump` for two reasons: a list of plain values
    keeps the flow form, because `consumers: [client]` is how it is written by hand, and a
    list of mappings is indented under its key, which safe_dump declines to do.
    """
    if _is_scalar(value):
        return [f"{key}: {_scalar_text(value)}"]
    if isinstance(value, (list, tuple)):
        if not value:
            return [f"{key}: []"]
        if all(_is_scalar(each) for each in value):
            flow = yaml.safe_dump(list(value), default_flow_style=True, width=_WIDE,
                                  allow_unicode=True).strip()
            return [f"{key}: {flow}"]
        lines = [f"{key}:"]
        for each in value:
            lines.extend(_render_item(each, 2))
        return lines
    if isinstance(value, dict):
        if not value:
            return [f"{key}: {{}}"]
        lines = [f"{key}:"]
        for sub_key, sub_value in value.items():
            lines.extend("  " + line for line in _render_field(sub_key, sub_value))
        return lines
    raise YamlEditError(f"'{key}': {type(value).__name__} is not something to write here")


def _render_mapping(mapping: Dict[str, Any]) -> List[str]:
    lines: List[str] = []
    for key, value in mapping.items():
        lines.extend(_render_field(key, value))
    return lines


def _render_item(item: Any, indent: int) -> List[str]:
    """One list entry: "- " and its body, all of it at `indent`."""
    body = _render_mapping(item) if isinstance(item, dict) else [_scalar_text(item)]
    return ([" " * indent + "- " + body[0]]
            + [" " * (indent + 2) + line for line in body[1:]])


def _put_key(lines: List[str], block: _Block, key: str, value: Any) -> List[str]:
    indent = block.child_indent
    rendered = [" " * indent + line for line in _render_field(key, value)]
    key_line = _find_key(lines, block, key)
    if key_line is None:
        at = _trimmed_end(lines, block.start, block.end)
        return lines[:at] + _spaced(lines, at, indent, rendered) + lines[at:]
    existing = _block_at(lines, key_line)
    end = _trimmed_end(lines, key_line + 1, existing.end)
    return lines[:key_line] + rendered + lines[end:]


def _spaced(lines: List[str], at: int, indent: int, rendered: List[str]) -> List[str]:
    """A blank line before a new top-level section, the way the rest of the file reads."""
    if indent == 0 and at > 0 and lines[at - 1].strip():
        return [""] + rendered
    return rendered
