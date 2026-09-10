# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""An entity's certificate lives beside the authority's own files, so no entity may be named
after them.
"""

from __future__ import annotations

import hashlib
import shutil

import pytest

from synqt import appmodel, check, mesh


def _digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.mark.skipif(shutil.which("openssl") is None, reason="openssl is not installed")
@pytest.mark.parametrize("name", ["ca", "CA", "docker-ca"])
def test_a_certificate_named_after_the_authority_is_refused_and_the_authority_kept(
        tmp_path, name):
    mesh.init(tmp_path)
    directory = tmp_path / "synqt" / "mesh"
    before = {path.name: _digest(path) for path in directory.iterdir() if path.is_file()}
    with pytest.raises(mesh.MeshError, match="certificate authority"):
        mesh.cert(tmp_path, name)
    after = {path.name: _digest(path) for path in directory.iterdir() if path.is_file()}
    assert after == before
    # And the authority still issues.
    assert "Issued web.crt" in mesh.cert(tmp_path, "web")


@pytest.mark.parametrize("name", ["ca", "Ca", "docker-ca"])
def test_check_refuses_an_entity_named_after_the_authority(name):
    config = {"entities": [{"name": "app", "type": "client"},
                           {"name": "edge", "type": "web_edge"},
                           {"name": name, "type": "service"}],
              "connect_points": [{"owner": "edge", "consumers": ["app"]}]}
    ok, messages = check.validate(config)
    assert not ok
    assert any("certificate authority" in message for message in messages), messages


def test_a_project_may_still_be_called_ca():
    # Only entity certificates sit in the mesh directory.
    assert appmodel.is_valid_project_name("ca")
    assert not appmodel.is_valid_entity_name("ca")
