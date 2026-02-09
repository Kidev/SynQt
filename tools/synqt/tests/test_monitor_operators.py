# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt monitor operator add`: the one gate on a console that shows everything."""

import hashlib

import pytest

from synqt import monitorops


def test_a_minted_credential_has_the_shape_the_monitor_parses():
    entry = monitorops.mint("ada", "correct horse battery")
    name, iterations, salt, digest = entry.split(":")
    assert name == "ada"
    assert int(iterations) >= monitorops.MINIMUM_ITERATIONS
    assert len(bytes.fromhex(salt)) == 16
    assert len(bytes.fromhex(digest)) == 32


def test_the_password_is_nowhere_in_what_is_printed():
    password = "correct horse battery"
    entry = monitorops.mint("ada", password)
    assert password not in entry
    assert password not in monitorops.instructions(entry)


def test_the_derivation_is_the_one_the_monitor_verifies_with():
    # Matches QPasswordDigestor::deriveKeyPbkdf2 with SHA-256;
    # tests/monitor/tst_operator.cpp verifies a credential minted here.
    salt = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    expected = hashlib.pbkdf2_hmac("sha256", b"correct horse battery", salt, 600000, 32)
    assert expected.hex() == (
        "bb06c8c0b1dd5bfd4e40f4e297a2d0e64da7ef94b4b8ec20989021c8b41536ad")


def test_every_credential_gets_its_own_salt():
    first = monitorops.mint("ada", "correct horse battery")
    second = monitorops.mint("bob", "correct horse battery")
    assert first.split(":")[2] != second.split(":")[2]
    assert first.split(":")[3] != second.split(":")[3]


def test_a_weak_iteration_count_is_raised_to_the_minimum():
    entry = monitorops.mint("ada", "correct horse battery", iterations=1000)
    assert int(entry.split(":")[1]) == monitorops.MINIMUM_ITERATIONS


def test_a_short_password_is_refused_rather_than_warned_about():
    with pytest.raises(monitorops.MonitorOpsError) as raised:
        monitorops.mint("ada", "short")
    assert "12 characters" in str(raised.value)


def test_a_name_that_would_not_survive_the_environment_is_refused():
    # The list is split on whitespace and colons, so a name may contain neither.
    for name in ("ada smith", "ada:smith", ""):
        with pytest.raises(monitorops.MonitorOpsError):
            monitorops.mint(name, "correct horse battery")


def test_the_instructions_name_the_variable_the_monitor_reads():
    entry = monitorops.mint("ada", "correct horse battery")
    printed = monitorops.instructions(entry)
    assert monitorops.CREDENTIAL_VARIABLE in printed
    assert "out of the repository" in printed
