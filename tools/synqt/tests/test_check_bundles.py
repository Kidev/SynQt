# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`bundles:` validation: who may download what, refused at check time and not in production."""

from synqt import appmodel, check


def _config(bundles, clients=("app",), scopes=("anonymous", "user"), identity=True):
    edge = {"name": "web", "type": "web_edge", "bundles": bundles}
    entities = [edge] + [{"name": name, "type": "client"} for name in clients]
    config = {"entities": entities,
              "scopes": {"order": list(scopes), "default": "anonymous"}}
    if identity:
        # A list, as appmodel.identity_providers reads it.
        config["identity"] = {"required": True,
                              "providers": [{"name": "github", "client_id": "x"}]}
    return config


def _edge_dir(root):
    # Derived from <type>/<name>/, as the generator does.
    return root / appmodel.entity_dir({"name": "web", "type": "web_edge"})


def _landing(root, name="landing"):
    directory = _edge_dir(root) / name
    directory.mkdir(parents=True)
    (directory / "index.html").write_text("<!doctype html>")
    return directory


def test_a_valid_block_passes(tmp_path):
    _landing(tmp_path)
    config = _config({"anonymous": "landing/", "user": "app"})
    assert check.lint_bundles(config, tmp_path) == []


def test_an_unknown_scope_is_refused():
    findings = check.lint_bundles(_config({"operator": "app", "anonymous": "app"}))
    assert any("operator" in f and "scope" in f for f in findings)


def test_an_unknown_client_entity_is_refused():
    findings = check.lint_bundles(_config({"anonymous": "console"}))
    assert any("console" in f for f in findings)


def test_a_missing_static_directory_is_refused(tmp_path):
    findings = check.lint_bundles(_config({"anonymous": "landing/"}), tmp_path)
    assert any("landing" in f for f in findings)


def test_a_static_directory_without_an_index_is_refused(tmp_path):
    (_edge_dir(tmp_path) / "landing").mkdir(parents=True)
    findings = check.lint_bundles(_config({"anonymous": "landing/"}), tmp_path)
    assert any("index.html" in f for f in findings)


def test_a_static_directory_escaping_the_entity_folder_is_refused(tmp_path):
    _edge_dir(tmp_path).mkdir(parents=True)
    findings = check.lint_bundles(_config({"anonymous": "../../etc/"}), tmp_path)
    assert any("outside" in f.lower() for f in findings)


def test_the_default_scope_must_resolve_to_something():
    findings = check.lint_bundles(_config({"user": "app"}))
    assert any("anonymous" in f and "first" in f.lower() for f in findings)


def test_a_desktop_only_client_may_not_be_a_bundle():
    config = _config({"anonymous": "app"})
    config["entities"][1]["targets"] = ["desktop"]
    findings = check.lint_bundles(config)
    assert any("wasm" in f for f in findings)


def test_an_ambiguous_value_is_refused(tmp_path):
    # A client named `app` and a directory `app/` both exist: refused, with how to
    # disambiguate.
    _landing(tmp_path, "app")
    findings = check.lint_bundles(_config({"anonymous": "app"}), tmp_path)
    assert any("ambiguous" in f.lower() for f in findings)


def test_an_unreachable_client_entity_warns():
    config = _config({"anonymous": "app"}, clients=("app", "gate"))
    findings = check.lint_bundles(config)
    assert any(f.startswith("warn:") and "gate" in f for f in findings)


def test_more_than_one_bundle_without_identity_warns():
    config = _config({"anonymous": "app", "user": "app"}, identity=False)
    findings = check.lint_bundles(config)
    assert any(f.startswith("warn:") and "identity" in f for f in findings)


def test_a_project_with_no_block_is_silent():
    config = {"entities": [{"name": "web", "type": "web_edge"},
                           {"name": "app", "type": "client"}],
              "scopes": {"order": ["anonymous"], "default": "anonymous"}}
    assert check.lint_bundles(config) == []
