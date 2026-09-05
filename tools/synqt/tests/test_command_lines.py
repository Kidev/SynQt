# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The commands the CLI decides to run, and what it refuses to run them for: `synqt docker`,
`synqt build --deploy` and `synqt test` decisions that need no Docker, CMake or Qt kit.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from synqt import build as buildmod
from synqt import cmakegen
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
    """Docker without the compose plugin is still a working installation."""
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
    """An unbuilt mounted bundle is refused before docker starts."""
    (generated / dockermod.COMPOSE_FILE).write_text(
        f"services:\n  web:\n    volumes: [./build/client:{dockermod.APP_DIR}/build/client:ro]\n")
    assert dockermod.client_is_mounted(generated) is True
    with pytest.raises(dockermod.DockerError) as refused:
        dockermod.up_command(generated)
    assert "synqt build --client wasm" in str(refused.value)
    (generated / "build" / "client").mkdir(parents=True)
    assert dockermod.up_command(generated)[-1] == "--build"


def test_every_mounted_bundle_must_be_built_before_docker_starts(generated):
    (generated / dockermod.COMPOSE_FILE).write_text(
        "services:\n  web:\n    volumes:\n"
        f"      - ./build/client-app:{dockermod.APP_DIR}/build/client-app:ro\n"
        f"      - ./build/client-admin:{dockermod.APP_DIR}/build/client-admin:ro\n")
    (generated / "build" / "client-app").mkdir(parents=True)
    with pytest.raises(dockermod.DockerError) as refused:
        dockermod.up_command(generated)
    assert "build/client-admin" in str(refused.value)
    (generated / "build" / "client-admin").mkdir()
    assert dockermod.up_command(generated)[-1] == "--build"


def test_a_compose_file_that_cannot_be_read_mounts_nothing(tmp_path):
    assert dockermod.client_is_mounted(tmp_path) is False


# Which targets a client selector resolves to


def _config(*entities):
    return {"entities": list(entities)}


def test_a_project_with_no_client_builds_only_its_services():
    config = _config({"name": "web", "type": "service"}, {"name": "db", "type": "service"})
    entities, host, client = buildmod._targets_for(config, "all")
    # Every client entity, not only the first.
    assert entities == []
    assert host == ["web", "db"]
    assert client == []


def test_a_browser_client_is_built_by_the_wasm_kit_and_not_with_the_services():
    config = _config({"name": "app", "type": "client", "targets": ["wasm"]},
                     {"name": "web", "type": "service"})
    entities, host, client = buildmod._targets_for(config, "wasm")
    assert [e["name"] for e in entities] == ["app"]
    assert host == ["web"]
    assert client == ["wasm"]


def test_a_desktop_client_is_built_by_the_host_kit_and_joins_the_services():
    """A desktop client builds with the host kit, alongside the services."""
    config = _config({"name": "app", "type": "client", "targets": ["wasm", "desktop"]},
                     {"name": "web", "type": "service"})
    _, host, client = buildmod._targets_for(config, "all")
    assert host == ["web", "app"]
    assert set(client) == {"wasm", "desktop"}


def test_two_clients_are_both_built():
    """The whole point of the list: a gate and an application both reach the compiler."""
    config = _config({"name": "app", "type": "client", "targets": ["wasm"]},
                     {"name": "gate", "type": "client", "targets": ["wasm"]},
                     {"name": "web", "type": "service"})
    entities, host, client = buildmod._targets_for(config, "wasm")
    assert [e["name"] for e in entities] == ["app", "gate"]
    assert host == ["web"]
    assert client == ["wasm"]


def test_a_desktop_client_beside_a_wasm_one_joins_the_host_build():
    config = _config({"name": "app", "type": "client", "targets": ["wasm"]},
                     {"name": "kiosk", "type": "client", "targets": ["desktop"]},
                     {"name": "web", "type": "service"})
    _, host, client = buildmod._targets_for(config, "all")
    assert host == ["web", "kiosk"]
    assert set(client) == {"wasm", "desktop"}


# What a finished deploy says to do next


def test_an_unsigned_tree_says_what_that_costs_on_this_platform(monkeypatch):
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "macos")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"), None)
    assert "UNSIGNED" in note
    assert "--sign" in note


def test_a_signed_macos_tree_is_told_the_one_step_synqt_does_not_run(monkeypatch):
    """The signed macOS note gives the exact notarization command."""
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "macos")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"),
                                    "Developer ID Application: Someone")
    assert "notarytool submit" in note
    assert "stapler staple" in note
    # Composed with the platform separator.
    assert str(Path("/out") / "app.app") in note


def test_a_signed_tree_anywhere_else_needs_nothing_further(monkeypatch):
    monkeypatch.setattr(buildmod, "desktop_platform", lambda: "windows")
    note = buildmod._deployed_note(Path("/project"), "app", Path("/out"), "a certificate")
    assert "Nothing further" in note
    assert "notarytool" not in note


# Which side of the system a changed file is on


def test_a_configuration_or_contract_change_rebuilds_both_sides(tmp_path):
    """A configuration or contract change rebuilds both sides."""
    config = _config({"name": "app", "type": "client"}, {"name": "web", "type": "service"})
    for changed in (tmp_path / "synqt.yaml", tmp_path / "shared" / "Auction.syn"):
        assert runmod._categorize({changed}, tmp_path, config) == (True, True)


def test_an_entitys_qml_is_attributed_to_that_entitys_side(tmp_path):
    config = _config({"name": "app", "type": "client"}, {"name": "web", "type": "service"})
    assert runmod._categorize({tmp_path / "client" / "app" / "Main.qml"}, tmp_path, config) \
        == (False, True)
    assert runmod._categorize({tmp_path / "service" / "web" / "Auction.qml"}, tmp_path,
                              config) == (True, False)


def test_an_entitys_qml_in_its_own_folder_is_attributed_to_that_entitys_side(tmp_path):
    """Entity QML under `<type>/<name>/` is attributed to that entity's side
    (`client/app/Main.qml` is the client).
    """
    config = _config({"name": "app", "type": "client"}, {"name": "edge", "type": "web_edge"},
                     {"name": "store", "type": "relational"})
    assert runmod._categorize({tmp_path / "client" / "app" / "Main.qml"}, tmp_path, config) \
        == (False, True)
    assert runmod._categorize({tmp_path / "client" / "app" / "views" / "Card.qml"}, tmp_path,
                              config) == (False, True)
    assert runmod._categorize({tmp_path / "web" / "edge" / "Edge.qml"}, tmp_path, config) \
        == (True, False)
    assert runmod._categorize({tmp_path / "db" / "relational" / "store" / "Store.qml"},
                              tmp_path, config) == (True, False)
    # Two entities of one type share the type folder and not each other's files.
    assert runmod._categorize({tmp_path / "web" / "edge2" / "Edge2.qml"}, tmp_path, config) \
        == (True, True)


def test_a_file_belonging_to_nothing_in_particular_rebuilds_both_sides(tmp_path):
    """A file no entity owns rebuilds both sides."""
    config = _config({"name": "app", "type": "client"}, {"name": "web", "type": "service"})
    assert runmod._categorize({Path("/elsewhere/Main.qml")}, tmp_path, config) == (True, True)
    assert runmod._categorize({tmp_path / "scratch" / "Main.qml"}, tmp_path, config) == \
        (True, True)


# What `synqt test` refuses to run


def test_a_machine_with_no_ctest_is_told_so(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(runmod.shutil, "which", lambda name: None)
    assert runmod.test(tmp_path) == 1
    assert "ctest not found" in capsys.readouterr().out


def test_a_project_with_no_tests_is_not_a_failure(tmp_path, monkeypatch, capsys):
    """No tests is not a failure, and ctest is not run."""
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
    """The test target is built, ctest runs it, and ctest's exit code is returned."""
    monkeypatch.setattr(runmod.shutil, "which", lambda name: f"/usr/bin/{name}")
    tests = tmp_path / "tests"
    tests.mkdir()
    (tests / "tst_Auction.qml").write_text("TestCase {}\n")
    host = tmp_path / "build" / "host-debug"
    host.mkdir(parents=True)
    (host / "CTestTestfile.cmake").write_text("")
    ran = []

    def remember(command, **named):
        ran.append(command)
        return subprocess.CompletedProcess(command, 3 if command[0] == "ctest" else 0)

    monkeypatch.setattr(runmod.subprocess, "run", remember)
    assert runmod.test(tmp_path) == 3
    assert ran[0][:2] == ["/usr/bin/cmake", "--build"]
    assert ran[0][-2:] == ["--target", cmakegen.TESTS_TARGET]
    assert ran[1][:2] == ["ctest", "--test-dir"]
    assert "--output-on-failure" in ran[1]


if __name__ == "__main__":
    pytest.main([__file__])


def _bundle_values(argv):
    return [argv[index + 1] for index, item in enumerate(argv) if item == "--bundle"]


def test_dev_passes_one_bundle_per_scope():
    config = {"entities": [{"name": "web", "type": "web_edge",
                            "bundles": {"anonymous": "landing/", "user": "app"}},
                           {"name": "app", "type": "client"},
                           {"name": "gate", "type": "client"}],
              "scopes": {"order": ["anonymous", "user"], "default": "anonymous"}}
    argv = runmod.dev_command(Path("/p"), config["entities"][0], config, 8443)
    values = _bundle_values(argv)
    # Built with Path, since arguments carry the host separator.
    assert f"anonymous={Path('/p') / 'web' / 'web' / 'landing'}" in values, values
    assert f"user={Path('/p') / 'build' / 'client-app'}" in values, values


def test_dev_passes_a_bare_bundle_for_a_project_with_no_block():
    # Without `bundles:` the argv is the bare directory, keyed by the default scope.
    config = {"entities": [{"name": "web", "type": "web_edge"},
                           {"name": "app", "type": "client"}]}
    argv = runmod.dev_command(Path("/p"), config["entities"][0], config, 8443)
    assert _bundle_values(argv) == [str(Path("/p") / "build" / "client")]


# The development scope picker's flag


def _edge_config():
    return {"entities": [{"name": "web", "type": "web_edge"},
                         {"name": "app", "type": "client"}]}


def test_dev_does_not_pass_the_picker_flag_unless_it_was_asked_for():
    config = _edge_config()
    argv = runmod.dev_command(Path("/p"), config["entities"][0], config, 8443)
    assert "--identity-picker" not in argv, argv
    # --dev stays; the picker is a choice within it.
    assert "--dev" in argv, argv


def test_dev_passes_the_picker_flag_when_asked():
    config = _edge_config()
    argv = runmod.dev_command(Path("/p"), config["entities"][0], config, 8443,
                              identity_picker=True)
    assert "--identity-picker" in argv, argv
    assert "--dev" in argv, argv


def test_named_identities_ride_along_with_the_picker(tmp_path):
    # Entries from `.dev-identities` arrive checked: one flag per usable entry, one per
    # problem.
    (tmp_path / ".dev-identities").write_text(
        "- email: alice@example.com\n  scope: admin\n"
        "- email: bob@example.com\n  scope: wizard\n")
    config = _edge_config()
    config["scopes"] = {"order": ["anonymous", "admin"]}
    argv = runmod.dev_command(tmp_path, config["entities"][0], config, 8443,
                              identity_picker=True)
    assert "--dev-identity=admin=alice@example.com" in argv, argv
    problems = [value for value in argv if value.startswith("--dev-identity-problem=")]
    assert len(problems) == 1, argv
    assert "wizard" in problems[0]


def test_a_project_with_no_dev_identities_passes_none(tmp_path):
    config = _edge_config()
    argv = runmod.dev_command(tmp_path, config["entities"][0], config, 8443,
                              identity_picker=True)
    assert not [value for value in argv if value.startswith("--dev-identity")], argv


def test_named_identities_are_not_read_without_the_picker(tmp_path):
    # Only an edge serving the picker gets the flags.
    (tmp_path / ".dev-identities").write_text(
        "- email: alice@example.com\n  scope: admin\n")
    config = _edge_config()
    config["scopes"] = {"order": ["anonymous", "admin"]}
    argv = runmod.dev_command(tmp_path, config["entities"][0], config, 8443)
    assert not [value for value in argv if value.startswith("--dev-identity")], argv


def test_only_the_edge_is_offered_the_picker():
    # A service does not declare the option and would exit on it.
    config = {"entities": [{"name": "db", "type": "relational"}]}
    argv = runmod.dev_command(Path("/p"), config["entities"][0], config, 8443,
                              identity_picker=True)
    assert "--identity-picker" not in argv, argv
