# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The release workflow's version step, and the quoting of every inline container script."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
RELEASE = WORKFLOWS / "release.yml"

needs_tools = pytest.mark.skipif(
    shutil.which("bash") is None or shutil.which("git") is None, reason="needs bash and git"
)


def _compute_step():
    workflow = yaml.safe_load(RELEASE.read_text(encoding="utf-8"))
    for step in workflow["jobs"]["version"]["steps"]:
        if step.get("id") == "compute":
            return step
    raise AssertionError("release.yml has no compute step")


def _repo(tmp_path, tags):
    repo = tmp_path / "repo"
    repo.mkdir()
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-C", str(repo)]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "c"], check=True)
    for tag in tags:
        subprocess.run(git + ["tag", tag], check=True)
    return repo


def _run(tmp_path, tags=(), bump="patch", version="", dry_run="false", skip_pypi="false"):
    repo = _repo(tmp_path, tags)
    output = tmp_path / "output"
    output.write_text("")
    env = dict(os.environ)
    env.update({
        "BUMP": bump,
        "CUSTOM_VERSION": version,
        "DRY_RUN": dry_run,
        "SKIP_PYPI": skip_pypi,
        "GITHUB_OUTPUT": str(output),
    })
    result = subprocess.run(["bash", "-e", "-c", _compute_step()["run"]], cwd=repo, env=env,
                            capture_output=True, text=True)
    values = dict(line.split("=", 1) for line in output.read_text().splitlines() if line)
    return result, values


def test_the_inputs_are_the_ones_the_step_reads():
    workflow = yaml.safe_load(RELEASE.read_text(encoding="utf-8"))
    # PyYAML reads the `on` key as True.
    inputs = workflow[True]["workflow_dispatch"]["inputs"]
    assert list(inputs) == ["bump", "version", "dry_run", "skip_pypi"]
    env = _compute_step()["env"]
    assert env["CUSTOM_VERSION"] == "${{ inputs.version }}"
    assert env["BUMP"] == "${{ inputs.bump }}"


@needs_tools
@pytest.mark.parametrize("tags,bump,expected", [
    ((), "patch", "0.0.1"),
    (("v0.3.7",), "patch", "0.3.8"),
    (("v0.3.7",), "minor", "0.4.0"),
    (("v0.3.7",), "major", "1.0.0"),
    (("v0.3.7", "v1.2.0-rc1"), "patch", "1.2.1"),
])
def test_a_bump_is_stable(tmp_path, tags, bump, expected):
    result, values = _run(tmp_path, tags=tags, bump=bump)
    assert result.returncode == 0, result.stdout + result.stderr
    assert values == {"version": expected, "tag": f"v{expected}", "prerelease": "false"}


@needs_tools
@pytest.mark.parametrize("custom,expected,prerelease", [
    ("2.0.0", "2.0.0", "false"),
    ("v2.0.0", "2.0.0", "false"),
    ("2.0.0-rc.1", "2.0.0-rc.1", "true"),
    ("2.0.0rc1", "2.0.0rc1", "true"),
    ("2.0.0-alpha", "2.0.0-alpha", "true"),
])
def test_a_custom_version_overrides_the_bump(tmp_path, custom, expected, prerelease):
    result, values = _run(tmp_path, tags=("v0.3.7",), bump="major", version=custom)
    assert result.returncode == 0, result.stdout + result.stderr
    assert values == {"version": expected, "tag": f"v{expected}", "prerelease": prerelease}


@needs_tools
@pytest.mark.parametrize("custom", ["2.0", "latest", "2.0.0 rc", "2.0.0;id", "2.0.0'", "-2.0.0"])
def test_a_malformed_custom_version_is_refused(tmp_path, custom):
    result, values = _run(tmp_path, version=custom)
    assert result.returncode != 0
    assert "custom version must be" in result.stdout
    assert values == {}


@needs_tools
def test_an_existing_tag_is_refused(tmp_path):
    result, values = _run(tmp_path, tags=("v0.3.6", "v0.3.7"), version="0.3.6")
    assert result.returncode != 0
    assert "tag v0.3.6 already exists" in result.stdout
    assert values == {}


def _run_blocks():
    for path in sorted(WORKFLOWS.glob("*.yml")):
        workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
        for name, job in (workflow.get("jobs") or {}).items():
            for step in job.get("steps") or []:
                if "run" in step:
                    yield f"{path.name}:{name}:{step.get('name', '')}", step["run"]


def test_no_single_quote_inside_a_single_quoted_script():
    offenders = []
    for where, script in _run_blocks():
        for opened in re.finditer(r"-c '\n", script):
            body = script[opened.end():]
            closing = re.search(r"^\s*'\s*$", body, re.MULTILINE)
            assert closing, f"{where}: unterminated single-quoted script"
            if "'" in body[:closing.start()]:
                offenders.append(where)
    assert offenders == []
