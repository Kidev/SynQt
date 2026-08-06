# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The commands the CLI decides to run, and what it refuses to run them for.

These are the parts of `synqt docker`, `synqt build --deploy` and `synqt test` that decide
something before any process starts: which compose binary exists, whether the tree is in a
state the command can work on, which targets a client selector resolves to, and what a
finished deploy tells you to do next. All of it is reachable without Docker, CMake or a Qt
kit, which is why it is worth holding down here rather than in a suite that needs all three.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from synqt import build as buildmod
from synqt import docker as dockermod
from synqt import run as runmod


# Which compose binary


def _no_binaries(monkeypatch, present=()):
    monkeypatch.setattr(dockermod.shutil, "which",
                        lambda name: f"/usr/bin/{name}" if name in present else None)


def _probe(monkeypatch, returncode):
    monkeypatch.setattr(dockermod.subprocess, "run",
                        lambda *a, **k: subprocess.CompletedProcess(a[0], returncode, "", ""))


def test_the_compose_plugin_is_preferred_when_docker_has_one(monkeypatch):
    _no_binaries(monkeypatch, present=("docker", "docker-compose"))
    _probe(monkeypatch, 0)
    assert dockermod.compose_command() == ["docker", "compose"]


def test_the_standalone_binary_is_used_when_the_plugin_is_not_there(monkeypatch):
    """Docker without the compose plugin is a real installation, not a broken one."""
    _no_binaries(monkeypatch, present=("docker", "docker-compose"))
    _probe(monkeypatch, 1)
    assert dockermod.compose_command() == ["docker-compose"]


def test_the_standalone_binary_is_used_when_there_is_no_docker_at_all(monkeypatch):
    _no_binaries(monkeypatch, present=("docker-compose",))
    assert dockermod.compose_command() == ["docker-compose"]


def test_no_compose_at_all_says_what_to_install(monkeypatch):
    _no_binaries(monkeypatch)
    with pytest.raises(dockermod.DockerError) as refused:
        dockermod.compose_command()
    assert "compose" in str(refused.value)


# What up and down refuse


@pytest.fixture
def generated(tmp_path, monkeypatch):
    """A project that has been through `synqt docker init`, as far as these commands care."""
    (tmp_path / dockermod.DOCKER_DIR).mkdir(parents=True)
    (tmp_path / dockermod.DOCKER_DIR / "Dockerfile").write_text("FROM scratch\n")
    (tmp_path / dockermod.COMPOSE_FILE).write_text("services: {}\n")
    _no_binaries(monkeypatch, present=("docker",))
    _probe(monkeypatch, 0)
    return tmp_path


def test_a_project_with_no_compose_file_is_told_to_init_first(tmp_path):
    with pytest.raises(dockermod.DockerError) as refused:
        dockermod.up_command(tmp_path)
    assert "synqt docker init" in str(refused.value)


def test_up_builds_and_stays_in_the_foreground_by_default(generated):
    assert dockermod.up_command(generated) == ["docker", "compose", "up", "--build"]


def test_up_takes_its_two_flags(generated):
    assert dockermod.up_command(generated, detach=True, build=False) == \
        ["docker", "compose", "up", "--detach"]


def test_down_takes_its_one_flag(generated):
    assert dockermod.down_command(generated) == ["docker", "compose", "down"]
    assert dockermod.down_command(generated, volumes=True) == \
        ["docker", "compose", "down", "--volumes"]


def test_a_mounted_bundle_that_has_not_been_built_is_refused_before_docker_starts(generated):
    """The compose file serves the bundle off the disk, so an empty build/client is a
    container that comes up serving nothing. Saying so now beats a blank page later."""
    (generated / dockermod.COMPOSE_FILE).write_text(
        f"services:\n  web:\n    volumes: [./build/client:{dockermod.APP_DIR}/build/client:ro]\n")
    assert dockermod.client_is_mounted(generated) is True
    with pytest.raises(dockermod.DockerError) as refused:
        dockermod.up_command(generated)
    assert "synqt build --client wasm" in str(refused.value)
    (generated / "build" / "client").mkdir(parents=True)
    assert dockermod.up_command(generated)[-1] == "--build"


def test_a_compose_file_that_cannot_be_read_mounts_nothing(tmp_path):
    assert dockermod.client_is_mounted(tmp_path) is False


# Which targets a client selector resolves to


def _config(*entities):
    return {"entities": list(entities)}


def test_a_project_with_no_client_builds_only_its_services():
    config = _config({"name": "web", "kind": "service"}, {"name": "db", "kind": "service"})
    entity, host, client = buildmod._targets_for(config, "all")
    assert entity is None
    assert host == ["web", "db"]
    assert client == []


def test_a_browser_client_is_built_by_the_wasm_kit_and_not_with_the_services():
    config = _config({"name": "app", "kind": "client", "targets": ["wasm"]},
                     {"name": "web", "kind": "service"})
    entity, host, client = buildmod._targets_for(config, "wasm")
    assert entity["name"] == "app"
    assert host == ["web"]
    assert client == ["wasm"]


def test_a_desktop_client_is_built_by_the_host_kit_and_joins_the_services():
    """It is the same QML, but the desktop target links the host kit, so it belongs to the
    build the services are in rather than to the one Emscripten drives."""
    config = _config({"name": "app", "kind": "client", "targets": ["wasm", "desktop"]},
                     {"name": "web", "kind": "service"})
    _, host, client = buildmod._targets_for(config, "all")
    assert host == ["web", "app"]
    assert set(client) == {"wasm", "desktop"}


# What a finished deploy says to do next


def test_an_unsigned_tree_says_what_that_costs_on_this_platform(monkeypatch):
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "macos")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"), None)
    assert "UNSIGNED" in note
    assert "--sign" in note


def test_a_signed_macos_tree_is_told_the_one_step_synqt_does_not_run(monkeypatch):
    """Notarization needs credentials and a network round trip, so the note hands over the
    exact command rather than leaving Gatekeeper to refuse the app on somebody's machine."""
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "macos")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"),
                                    "Developer ID Application: Someone")
    assert "notarytool submit" in note
    assert "stapler staple" in note
    assert "/out/app.app" in note


def test_a_signed_tree_anywhere_else_needs_nothing_further(monkeypatch):
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "windows")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"), "a certificate")
    assert "Nothing further" in note
    assert "notarytool" not in note


# Which side of the system a changed file is on


def test_a_configuration_or_contract_change_rebuilds_both_sides(tmp_path):
    """Both sides read them, so attributing either to one side would leave the other
    running against a topology or a contract that has moved."""
    config = _config({"name": "app", "kind": "client"}, {"name": "web", "kind": "service"})
    for changed in (tmp_path / "synqt.yaml", tmp_path / "shared" / "Auction.syn"):
        assert runmod._categorize({changed}, tmp_path, config) == (True, True)


def test_an_entitys_qml_is_attributed_to_that_entitys_side(tmp_path):
    config = _config({"name": "app", "kind": "client"}, {"name": "web", "kind": "service"})
    assert runmod._categorize({tmp_path / "app" / "Main.qml"}, tmp_path, config) == \
        (False, True)
    assert runmod._categorize({tmp_path / "web" / "Auction.qml"}, tmp_path, config) == \
        (True, False)


def test_a_file_belonging_to_nothing_in_particular_rebuilds_both_sides(tmp_path):
    """A file outside the tree, or in a directory no entity owns, is a change nobody can
    attribute; rebuilding both is the answer that cannot be wrong."""
    config = _config({"name": "app", "kind": "client"}, {"name": "web", "kind": "service"})
    assert runmod._categorize({Path("/elsewhere/Main.qml")}, tmp_path, config) == (True, True)
    assert runmod._categorize({tmp_path / "scratch" / "Main.qml"}, tmp_path, config) == \
        (True, True)


# What `synqt test` refuses to run


def test_a_machine_with_no_ctest_is_told_so(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(runmod.shutil, "which", lambda name: None)
    assert runmod.test(tmp_path) == 1
    assert "ctest not found" in capsys.readouterr().out


def test_a_project_with_no_tests_is_not_a_failure(tmp_path, monkeypatch, capsys):
    """It is the ordinary state of a new project. Reaching ctest reported a passing run
    over zero tests, which reads exactly like a suite that ran."""
    monkeypatch.setattr(runmod.shutil, "which", lambda name: "/usr/bin/ctest")
    assert runmod.test(tmp_path) == 0
    printed = capsys.readouterr().out
    assert "no tests yet" in printed
    assert "https://synqt.org/testing/" in printed


def test_tests_with_no_configured_build_say_which_command_configures_it(tmp_path,
                                                                       monkeypatch, capsys):
    monkeypatch.setattr(runmod.shutil, "which", lambda name: "/usr/bin/ctest")
    tests = tmp_path / "tests"
    tests.mkdir()
    (tests / "tst_Auction.qml").write_text("TestCase {}\n")
    assert runmod.test(tmp_path) == 1
    assert "synqt build" in capsys.readouterr().out


def test_a_configured_build_reaches_ctest_and_returns_what_it_said(tmp_path, monkeypatch):
    monkeypatch.setattr(runmod.shutil, "which", lambda name: "/usr/bin/ctest")
    tests = tmp_path / "tests"
    tests.mkdir()
    (tests / "tst_Auction.qml").write_text("TestCase {}\n")
    host = tmp_path / "build" / "host"
    host.mkdir(parents=True)
    (host / "CTestTestfile.cmake").write_text("")
    ran = []

    def remember(command, **named):
        ran.append(command)
        return subprocess.CompletedProcess(command, 3)

    monkeypatch.setattr(runmod.subprocess, "run", remember)
    assert runmod.test(tmp_path) == 3
    assert ran[0][:2] == ["ctest", "--test-dir"]
    assert "--output-on-failure" in ran[0]


if __name__ == "__main__":
    pytest.main([__file__])
