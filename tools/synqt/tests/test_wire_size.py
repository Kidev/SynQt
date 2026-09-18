# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor's wire sizes are bounds on what QtRemoteObjects really sends.

`assets/design/wire.js` says "at most N" beside each member. The packets below are laid out
the way QDataStreamCodec in qtremoteobjects writes them (qremoteobjectpacket.cpp): a quint32
length and a quint16 id, then per packet type:

- property change: the source name, the property index, the value as a QVariant;
- invoke (a signal emitted or a slot called): the name, the call type, the method index, a
  quint32 argument count, each argument as a QVariant, the serial id, the property index;
- invoke reply: the name, the acknowledged serial id, the value as a QVariant.

A QString is a quint32 byte count and UTF-16; a QVariant is a quint32 type id, a quint8 null
flag and the value.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

DESIGN = Path(__file__).resolve().parents[1] / "synqt" / "assets" / "design"

HEAD = 4 + 2
VARIANT = 4 + 1


def _string(characters):
    return 4 + 2 * characters


def _invoke(owner, arguments):
    return (HEAD + _string(len(owner)) + 4 + 4 + 4
            + sum(VARIANT + size for size in arguments) + 4 + 4)


def _reply(owner, value):
    return HEAD + _string(len(owner)) + 4 + VARIANT + value


def _change(owner, value):
    return HEAD + _string(len(owner)) + 4 + VARIANT + value


def _member_bytes(member, owner="edge"):
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    script = (f'import {{memberBytes}} from "{(DESIGN / "wire.js").as_uri()}";\n'
              f"console.log(JSON.stringify(memberBytes({json.dumps(member)}, "
              f"{{owner: {json.dumps(owner)}}})));\n")
    finished = subprocess.run(["node", "--input-type=module"], input=script,
                              capture_output=True, text=True, encoding="utf-8", check=False)
    assert finished.returncode == 0, finished.stderr
    return json.loads(finished.stdout)


def test_a_slot_call_and_its_answer_are_bounded_by_what_is_sent():
    member = {"kind": "slot", "name": "add", "type": "int",
              "params": [{"name": "text", "type": "string[10]"},
                         {"name": "count", "type": "int"}]}
    sent = _invoke("edge", [_string(10), 4]) + _reply("edge", 4)
    cost = _member_bytes(member)
    assert cost["bounded"]
    assert cost["bytes"] == sent


def test_a_signal_is_bounded_by_the_invoke_packet():
    member = {"kind": "signal", "name": "changed",
              "params": [{"name": "value", "type": "double"}]}
    assert _member_bytes(member)["bytes"] == _invoke("edge", [8])


def test_a_signal_with_no_parameters_still_costs_a_packet():
    member = {"kind": "signal", "name": "ping", "params": []}
    assert _member_bytes(member)["bytes"] == _invoke("edge", [])


def test_a_property_change_is_bounded_by_the_change_packet():
    member = {"kind": "prop", "name": "count", "type": "int"}
    assert _member_bytes(member)["bytes"] == _change("edge", 4)
