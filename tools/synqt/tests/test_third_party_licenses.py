# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a service entity's THIRD-PARTY-LICENSES lists from outside Qt.

The redis and mongodb providers compile into the providers library when their client
libraries are installed, and every service links it. The configure step records what it
linked and the notice reads that record.
"""

import re
from pathlib import Path

from synqt import licenses

REPO = Path(__file__).resolve().parents[3]


def service(name="books", provider=None):
    entity = {"name": name, "type": "relational"}
    if provider:
        entity["provider"] = {"name": provider}
    return entity


def test_the_record_is_read_by_the_names_the_notices_use(tmp_path):
    (tmp_path / licenses.PROVIDER_LIBRARIES_FILE).write_text("hiredis\nmongoc\n")
    assert licenses.provider_libraries(tmp_path) == [
        "hiredis", "MongoDB C driver (libmongoc, libbson)"]


def test_no_record_lists_nothing_rather_than_guessing(tmp_path):
    assert licenses.provider_libraries(tmp_path) == []


def test_a_service_lists_what_the_providers_library_linked():
    text = licenses.generate(service(), config={},
                             linked=["hiredis", "MongoDB C driver (libmongoc, libbson)"])
    assert "hiredis: BSD-3-Clause" in text
    assert "MongoDB C driver (libmongoc, libbson): Apache-2.0" in text


def test_a_browser_client_never_lists_them():
    client = {"name": "app", "type": "client"}
    assert licenses.entity_third_party(client, {}, "wasm", linked=["hiredis"]) == []


def test_a_postgres_entity_lists_the_client_library_its_driver_loads():
    text = licenses.generate(service(provider="postgres"), config={})
    assert "libpq (loaded by Qt's QPSQL plugin): PostgreSQL" in text


def test_every_name_the_build_records_has_a_license():
    """Every recorded name has a license entry."""
    cmake = (REPO / "src" / "providers" / "CMakeLists.txt").read_text(encoding="utf-8")
    recorded = set(re.findall(r"list\(APPEND _synqt_provider_libraries (\w+)\)", cmake))
    assert recorded, "the providers CMakeLists records nothing any more"
    assert recorded <= set(licenses.PROVIDER_LIBRARY_NAMES), recorded
    for listed in licenses.PROVIDER_LIBRARY_NAMES.values():
        assert listed in licenses._THIRD_PARTY, listed
