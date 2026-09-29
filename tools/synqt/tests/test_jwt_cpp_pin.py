# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""One jwt-cpp is pinned, and every workflow and the container image clone that one.

The tag is cloned rather than packaged, so nothing but this test notices a copy that moved.
"""

from __future__ import annotations

import re
from pathlib import Path

import yaml

from synqt import docker

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"

_PIN = re.compile(r"JWT_CPP_VERSION:\s*\"?([^\"\s]+)\"?")


def test_every_workflow_clones_the_pinned_jwt_cpp():
    found = {}
    for workflow in sorted(WORKFLOWS.glob("*.yml")):
        for tag in _PIN.findall(workflow.read_text(encoding="utf-8")):
            if not tag.startswith("$"):
                found.setdefault(workflow.name, set()).add(tag)
    assert found, "no workflow names JWT_CPP_VERSION"
    drifted = {name: tags for name, tags in found.items() if tags != {docker.JWT_CPP_VERSION}}
    assert not drifted, (f"jwt-cpp is pinned to {docker.JWT_CPP_VERSION} "
                         f"(synqt/docker.py); these workflows name another tag: {drifted}")


def test_the_container_image_clones_the_pinned_jwt_cpp():
    config = yaml.safe_load((ROOT / "examples" / "gavel" / "synqt.yaml").read_text(
        encoding="utf-8"))
    dockerfile = docker.render_dockerfile(config)
    assert f"--branch {docker.JWT_CPP_VERSION} " in dockerfile
