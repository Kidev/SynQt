# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What each `synqt` command actually calls, and what it does when that fails.

Every other test in this directory drives a module directly. This one drives the entry
point, because the wiring between the two is real code with real decisions in it: which
command reaches which function, what is passed along with it, when a command refuses to
continue, and which failures come back as a message and an exit code rather than a
traceback. None of that is exercised by calling `build.build()` yourself.

The work each command does is stubbed. The point here is the dispatch, not a second copy
of the tests that already cover the scaffolders, the builder, and the validator.
"""

from __future__ import annotations

import io
import textwrap
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import pytest

from synqt import (addauth, addcontract, addentity, addprovider, build as buildmod,
                   check as checkmod, cli, config as configmod, design as designmod,
                   doctor, mesh, newproject, run as runmod)

_PROJECT = textwrap.dedent("""\
    project:
      name: acme
    entities:
      - name: client
        kind: client
      - name: web
        kind: service
        capability: web_edge
      - name: database
        kind: service
""")


def _project(tmp_path: Path) -> str:
    (tmp_path / "synqt.yaml").write_text(_PROJECT, encoding="utf-8")
    return str(tmp_path)


def _run(argv):
    """Run the CLI, returning (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = cli.main(argv)
    return code, out.getvalue(), err.getvalue()


class TestNoCommand:
    def test_no_command_prints_help_and_exits_two(self):
        code, out, _ = _run([])
        assert code == 2
        assert "usage: synqt" in out

    def test_version_prints_three_lines_and_exits(self):
        out = io.StringIO()
        with redirect_stdout(out), pytest.raises(SystemExit) as exit_info:
            cli.main(["--version"])
        assert exit_info.value.code == 0
        assert len(out.getvalue().strip().splitlines()) == 3


class TestSimpleCommands:
    def test_new_scaffolds_with_the_flags_it_was_given(self, tmp_path, monkeypatch):
        seen = {}

        def scaffold(parent_dir, name, auth=None, blueprints=None):
            seen.update(parent_dir=parent_dir, name=name, auth=auth, blueprints=blueprints)
            return "scaffolded"

        monkeypatch.setattr(newproject, "scaffold", scaffold)
        code, out, _ = _run(["new", "acme", "--parent-dir", str(tmp_path),
                             "--auth", "github", "--blueprint", "persistence",
                             "--blueprint", "cache"])
        assert code == 0
        assert "scaffolded" in out
        assert seen == {"parent_dir": str(tmp_path), "name": "acme", "auth": "github",
                        "blueprints": ["persistence", "cache"]}

    def test_providers_lists_them_without_needing_a_project(self, monkeypatch):
        monkeypatch.setattr(addentity, "list_providers", lambda: "persistence: sqlite")
        code, out, _ = _run(["providers"])
        assert code == 0
        assert "persistence: sqlite" in out

    def test_doctor_reports_for_the_project_and_profile_given(self, tmp_path, monkeypatch):
        seen = {}

        def report(project_dir, profile=None):
            seen.update(project_dir=project_dir, profile=profile)
            return "all good"

        monkeypatch.setattr(doctor, "report", report)
        code, out, _ = _run(["doctor", "--project-dir", str(tmp_path), "--profile", "ci"])
        assert code == 0
        assert "all good" in out
        assert seen == {"project_dir": str(tmp_path), "profile": "ci"}

    def test_check_exits_one_when_the_project_does_not_check_out(self, tmp_path, monkeypatch):
        monkeypatch.setattr(checkmod, "check_project",
                            lambda *a, **k: (False, ["error: something"]))
        code, out, _ = _run(["check", "--project-dir", str(tmp_path)])
        assert code == 1
        assert "error: something" in out

    def test_check_passes_the_release_flag_through(self, tmp_path, monkeypatch):
        seen = {}

        def check_project(project_dir, release=False, types="auto", profile=None):
            seen.update(release=release, types=types, profile=profile)
            return True, ["ok"]

        monkeypatch.setattr(checkmod, "check_project", check_project)
        assert _run(["check", "--project-dir", str(tmp_path), "--release"])[0] == 0
        assert seen["release"] is True
        assert seen["types"] == "auto"

    def test_check_passes_the_type_backend_through(self, tmp_path, monkeypatch):
        seen = {}

        def check_project(project_dir, release=False, types="auto", profile=None):
            seen.update(types=types)
            return True, ["ok"]

        monkeypatch.setattr(checkmod, "check_project", check_project)
        assert _run(["check", "--project-dir", str(tmp_path),
                     "--types", "heuristic"])[0] == 0
        assert seen["types"] == "heuristic"

    def test_test_returns_whatever_the_runner_returns(self, tmp_path, monkeypatch):
        monkeypatch.setattr(runmod, "test", lambda project_dir: 3)
        assert _run(["test", "--project-dir", str(tmp_path)])[0] == 3


class TestClean:
    def test_clean_removes_build_and_keeps_the_toolchain_and_the_ca(self, tmp_path):
        (tmp_path / "build" / "web").mkdir(parents=True)
        (tmp_path / "synqt" / "mesh").mkdir(parents=True)
        (tmp_path / "synqt" / "mesh" / "ca.pem").write_text("x", encoding="utf-8")
        (tmp_path / "synqt" / "toolchain").mkdir(parents=True)

        code, out, _ = _run(["clean", "--project-dir", str(tmp_path)])
        assert code == 0
        assert not (tmp_path / "build").exists()
        assert (tmp_path / "synqt" / "mesh" / "ca.pem").exists()
        assert (tmp_path / "synqt" / "toolchain").exists()
        assert "build/" in out

    def test_clean_on_a_project_that_was_never_built_is_not_an_error(self, tmp_path):
        assert _run(["clean", "--project-dir", str(tmp_path)])[0] == 0


class TestAdd:
    def test_add_auth_forwards_required_and_the_provider_entity(self, tmp_path, monkeypatch):
        seen = {}

        def scaffold(project_dir, provider, required=False, provider_entity=""):
            seen.update(provider=provider, required=required, provider_entity=provider_entity)
            return "auth added"

        monkeypatch.setattr(addauth, "scaffold", scaffold)
        code, out, _ = _run(["add", "auth", "github", "--project-dir", str(tmp_path),
                             "--required", "--provider-entity", "auth"])
        assert code == 0
        assert "auth added" in out
        assert seen == {"provider": "github", "required": True, "provider_entity": "auth"}

    def test_add_entity_defaults_to_the_plain_service_blueprint(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(addentity, "scaffold",
                            lambda project_dir, name, blueprint, provider=None, source=None:
                            seen.update(name=name, blueprint=blueprint, provider=provider,
                                        source=source) or "entity added")
        assert _run(["add", "entity", "jobs", "--project-dir", str(tmp_path)])[0] == 0
        assert seen == {"name": "jobs", "blueprint": "service", "provider": None,
                        "source": None}

    def test_add_entity_passes_the_source_name_through_when_it_is_given(self, tmp_path,
                                                                       monkeypatch):
        seen = {}
        monkeypatch.setattr(addentity, "scaffold",
                            lambda project_dir, name, blueprint, provider=None, source=None:
                            seen.update(source=source) or "entity added")
        assert _run(["add", "entity", "jobs", "--source", "Rollups",
                     "--project-dir", str(tmp_path)])[0] == 0
        assert seen == {"source": "Rollups"}

    def test_add_provider_requires_and_forwards_a_family(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(addprovider, "scaffold",
                            lambda project_dir, name, family:
                            seen.update(name=name, family=family) or "provider added")
        assert _run(["add", "provider", "Acme", "--project-dir", str(tmp_path),
                     "--family", "cache"])[0] == 0
        assert seen == {"name": "Acme", "family": "cache"}

    def test_add_contract_reaches_the_contract_scaffolder(self, tmp_path, monkeypatch):
        monkeypatch.setattr(addcontract, "scaffold_contract",
                            lambda project_dir, name: f"contract {name}")
        code, out, _ = _run(["add", "contract", "Todo", "--project-dir", str(tmp_path)])
        assert code == 0
        assert "contract Todo" in out

    def test_add_connect_point_splits_the_consumer_list(self, tmp_path, monkeypatch):
        seen = {}

        def scaffold(project_dir, name, owner=None, consumers=None, contract=None,
                     instance=None):
            seen.update(name=name, owner=owner, consumers=consumers, contract=contract,
                        instance=instance)
            return "connect point added"

        monkeypatch.setattr(addcontract, "scaffold_connect_point", scaffold)
        assert _run(["add", "connect-point", "todo", "--project-dir", str(tmp_path),
                     "--owner", "web", "--contract", "Todo",
                     "--consumers", "client,database", "--instance", "per_session"])[0] == 0
        assert seen == {"name": "todo", "owner": "web", "consumers": ["client", "database"],
                        "contract": "Todo", "instance": "per_session"}

    def test_an_empty_consumer_list_is_no_consumers_not_one_empty_name(self, tmp_path,
                                                                      monkeypatch):
        seen = {}
        monkeypatch.setattr(addcontract, "scaffold_connect_point",
                            lambda project_dir, name, **kwargs:
                            seen.update(kwargs) or "added")
        assert _run(["add", "connect-point", "scores", "--project-dir", str(tmp_path),
                     "--owner", "database", "--contract", "Scores"])[0] == 0
        assert seen["consumers"] == []


class TestMesh:
    def test_mesh_init_forwards_force(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(mesh, "init",
                            lambda project_dir, force=False, dev=False:
                            seen.update(force=force) or "ca created")
        code, out, _ = _run(["mesh", "init", "--project-dir", _project(tmp_path), "--force"])
        assert code == 0
        assert "ca created" in out
        assert seen == {"force": True}

    def test_mesh_cert_all_is_given_every_service_entity_and_no_client(self, tmp_path,
                                                                      monkeypatch):
        seen = {}
        monkeypatch.setattr(mesh, "cert_all",
                            lambda project_dir, entities, dev=False:
                            seen.update(entities=entities) or "certs issued")
        assert _run(["mesh", "cert", "--all",
                     "--project-dir", _project(tmp_path)])[0] == 0
        assert seen["entities"] == ["web", "database"]

    def test_mesh_cert_for_one_entity_names_it(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(mesh, "cert",
                            lambda project_dir, entity: seen.update(entity=entity) or "issued")
        assert _run(["mesh", "cert", "web", "--project-dir", _project(tmp_path)])[0] == 0
        assert seen == {"entity": "web"}

    def test_mesh_cert_with_neither_an_entity_nor_all_says_so(self, tmp_path):
        code, _, err = _run(["mesh", "cert", "--project-dir", _project(tmp_path)])
        assert code == 1
        assert "--all" in err

    def test_mesh_rotate_and_status_reach_their_functions(self, tmp_path, monkeypatch):
        monkeypatch.setattr(mesh, "rotate", lambda project_dir, entity, entities: "rotated")
        monkeypatch.setattr(mesh, "status", lambda project_dir: "one CA, two certs")
        project = _project(tmp_path)
        assert "rotated" in _run(["mesh", "rotate", "--project-dir", project])[1]
        assert "one CA" in _run(["mesh", "status", "--project-dir", project])[1]


class TestBuildAndDev:
    def test_build_refuses_an_invalid_configuration_before_compiling_anything(
            self, tmp_path, monkeypatch):
        built = []
        monkeypatch.setattr(checkmod, "validate",
                            lambda *a, **k: (False, ["error: plaintext edge in release"]))
        monkeypatch.setattr(buildmod, "build", lambda *a, **k: built.append(True) or "built")

        code, _, err = _run(["build", "--project-dir", _project(tmp_path)])
        assert code == 1
        assert not built, "the build must not start after validation failed"
        assert "plaintext edge in release" in err
        assert "invalid configuration" in err

    def test_build_forwards_its_flags_and_is_release_by_default(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (True, []))
        monkeypatch.setattr(buildmod, "build",
                            lambda project_dir, **kwargs: seen.update(kwargs) or "built")

        assert _run(["build", "--project-dir", _project(tmp_path),
                     "--client", "desktop", "--entity", "web", "--threads", "multi"])[0] == 0
        assert seen["release"] is True
        assert seen["client"] == "desktop"
        assert seen["entity"] == "web"
        assert seen["threads"] == "multi"

    def test_debug_beats_the_release_default(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (True, []))
        monkeypatch.setattr(buildmod, "build",
                            lambda project_dir, **kwargs: seen.update(kwargs) or "built")
        assert _run(["build", "--project-dir", _project(tmp_path), "--debug"])[0] == 0
        assert seen["release"] is False

    def test_dev_issues_the_development_ca_before_it_validates(self, tmp_path, monkeypatch):
        order = []
        monkeypatch.setattr(mesh, "init",
                            lambda project_dir, dev=False, force=False: order.append("ca"))
        monkeypatch.setattr(mesh, "cert_all",
                            lambda project_dir, entities, dev=False: order.append("certs"))

        def validate(*a, **k):
            order.append("validate")
            return True, []

        monkeypatch.setattr(checkmod, "validate", validate)
        monkeypatch.setattr(buildmod, "build", lambda *a, **k: order.append("build") or "built")
        monkeypatch.setattr(runmod, "dev", lambda *a, **k: order.append("dev") or "serving")

        code, out, _ = _run(["dev", "--project-dir", _project(tmp_path)])
        assert code == 0
        assert order == ["ca", "certs", "validate", "build", "dev"], (
            "the certificate rule must see the certificates dev is about to create")
        assert "serving" in out

    def test_dev_is_a_debug_build_and_passes_its_run_flags_on(self, tmp_path, monkeypatch):
        built, ran = {}, {}
        monkeypatch.setattr(mesh, "init", lambda *a, **k: None)
        monkeypatch.setattr(mesh, "cert_all", lambda *a, **k: None)
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (True, []))
        monkeypatch.setattr(buildmod, "build",
                            lambda project_dir, **kwargs: built.update(kwargs) or "built")
        monkeypatch.setattr(runmod, "dev",
                            lambda project_dir, **kwargs: ran.update(kwargs) or "serving")

        assert _run(["dev", "--project-dir", _project(tmp_path),
                     "--port", "9001", "--no-open", "--no-watch"])[0] == 0
        assert built["release"] is False
        assert ran["port"] == 9001
        assert ran["open_browser"] is False
        assert ran["watch"] is False

    def test_a_directory_that_is_not_a_project_is_left_to_the_command_to_report(
            self, tmp_path, monkeypatch):
        # No synqt.yaml: validation has nothing to read, so it must not be the thing that
        # reports the problem. The builder is, in its own words.
        monkeypatch.setattr(buildmod, "build",
                            lambda *a, **k: (_ for _ in ()).throw(
                                buildmod.BuildError("no synqt.yaml here")))
        code, _, err = _run(["build", "--project-dir", str(tmp_path)])
        assert code == 1
        assert "synqt build: no synqt.yaml here" in err


class TestServe:
    def test_serve_holds_the_project_to_the_release_rules(self, tmp_path, monkeypatch):
        seen = {}

        def validate(config, release=False, project_dir=None, starting=False):
            seen.update(release=release, starting=starting)
            return True, []

        monkeypatch.setattr(checkmod, "validate", validate)
        monkeypatch.setattr(runmod, "serve", lambda *a, **k: "serving")
        assert _run(["serve", "--project-dir", _project(tmp_path)])[0] == 0
        assert seen == {"release": True, "starting": True}

    def test_serve_refuses_to_start_an_invalid_deployment(self, tmp_path, monkeypatch):
        started = []
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (False, ["error: nope"]))
        monkeypatch.setattr(runmod, "serve", lambda *a, **k: started.append(True) or "serving")
        assert _run(["serve", "--project-dir", _project(tmp_path)])[0] == 1
        assert not started


class TestDesign:
    def test_design_is_a_subcommand_with_its_flags(self):
        args = cli.build_parser().parse_args(["design", "--port", "9000", "--no-open"])
        assert args.command == "design"
        assert args.port == 9000
        assert args.no_open is True

    def test_design_serves_the_project_it_was_pointed_at(self, tmp_path, monkeypatch):
        seen = {}
        monkeypatch.setattr(designmod, "serve",
                            lambda project_dir, **kwargs:
                            seen.update(project_dir=project_dir, **kwargs) or "stopped")
        project = _project(tmp_path)
        assert _run(["design", "--project-dir", project, "--profile", "ci",
                     "--port", "9000", "--no-open"])[0] == 0
        assert seen == {"project_dir": project, "port": 9000, "open_browser": False,
                        "profile": "ci"}

    def test_design_opens_a_project_that_does_not_check_out(self, tmp_path, monkeypatch):
        # Deliberate: a topology the validator refuses is exactly what somebody opens the
        # editor to fix. Validating first would lock the one tool that repairs it behind the
        # damage. The page shows the same verdict on arrival, and Apply is what the rules
        # gate, not the door.
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (False, ["error: nope"]))
        served = []
        monkeypatch.setattr(designmod, "serve", lambda *a, **k: served.append(True) or "done")
        assert _run(["design", "--project-dir", _project(tmp_path)])[0] == 0
        assert served

    def test_a_design_failure_is_a_message_and_an_exit_code(self, tmp_path, monkeypatch):
        def explode(*a, **k):
            raise designmod.DesignError("port 8181 is already in use")

        monkeypatch.setattr(designmod, "serve", explode)
        code, _, err = _run(["design", "--project-dir", _project(tmp_path)])
        assert code == 1
        assert "synqt design: port 8181 is already in use" in err
        assert "Traceback" not in err


class TestErrorReporting:
    @pytest.mark.parametrize("error", [
        newproject.NewProjectError("bad name"),
        addauth.AddAuthError("unknown provider"),
        addentity.AddEntityError("unknown blueprint"),
        addprovider.AddProviderError("unknown family"),
        addcontract.AddContractError("no such entity"),
        mesh.MeshError("no CA yet"),
        designmod.DesignError("port already in use"),
        buildmod.BuildError("compile failed"),
        configmod.ConfigError("bad yaml"),
        FileNotFoundError("no such file"),
    ])
    def test_every_declared_failure_is_a_message_and_an_exit_code(self, tmp_path,
                                                                  monkeypatch, error):
        def explode(*a, **k):
            raise error

        monkeypatch.setattr(newproject, "scaffold", explode)
        code, _, err = _run(["new", "acme", "--parent-dir", str(tmp_path)])
        assert code == 1
        assert err.startswith("synqt new: ")
        assert "Traceback" not in err

    def test_a_failure_nobody_declared_is_not_swallowed(self, tmp_path, monkeypatch):
        # An unexpected exception has to reach the developer with its traceback intact.
        # Turning every failure into "synqt new: ..." would hide the bugs worth seeing.
        def explode(*a, **k):
            raise RuntimeError("something nobody planned for")

        monkeypatch.setattr(newproject, "scaffold", explode)
        with pytest.raises(RuntimeError):
            cli.main(["new", "acme", "--parent-dir", str(tmp_path)])


class TestValidationReporting:
    def test_a_layer_over_synqt_yaml_is_named_before_the_verdict(self, tmp_path,
                                                                 monkeypatch):
        # Which files were layered is the first thing to check when a build refuses a
        # configuration that looks fine in synqt.yaml alone, so every layer beyond the base
        # file announces itself.
        project = _project(tmp_path)
        (tmp_path / "synqt.ci.yaml").write_text("project:\n  name: acme-ci\n", encoding="utf-8")
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (True, ["warn: careful"]))
        monkeypatch.setattr(buildmod, "build", lambda *a, **k: "built")
        code, out, _ = _run(["build", "--project-dir", project, "--profile", "ci"])
        assert code == 0
        assert "synqt: synqt.ci.yaml" in out
        assert "warn: careful" in out

    def test_synqt_yaml_on_its_own_announces_nothing(self, tmp_path, monkeypatch):
        # The base file is not news. Naming it on every build would train people to skip
        # the line that matters when there really is a layer over it.
        monkeypatch.setattr(checkmod, "validate", lambda *a, **k: (True, []))
        monkeypatch.setattr(buildmod, "build", lambda *a, **k: "built")
        _, out, _ = _run(["build", "--project-dir", _project(tmp_path)])
        assert not any(line.startswith("synqt: ") for line in out.splitlines())

    def test_a_note_that_is_neither_an_error_nor_a_warning_is_not_printed(self, tmp_path,
                                                                         monkeypatch):
        monkeypatch.setattr(checkmod, "validate",
                            lambda *a, **k: (True, ["ok: nothing to say"]))
        monkeypatch.setattr(buildmod, "build", lambda *a, **k: "built")
        _, out, err = _run(["build", "--project-dir", _project(tmp_path)])
        assert "nothing to say" not in out
        assert "nothing to say" not in err
