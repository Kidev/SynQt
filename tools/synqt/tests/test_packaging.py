# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a published `synqt` has to be: the distribution metadata, and the framework sources
it carries so that an install with no SynQt checkout anywhere can still scaffold a project.

The wheel is built by CI, so these are the properties that can be asserted without building
one: that pyproject.toml says what the release depends on it saying, and that
`appmodel.framework_root` resolves the three roots in the order that makes a checkout win
over a packaged copy and makes a copy that will not outlive the process lose to an error.
"""

import glob
import sys
from pathlib import Path

import pytest

from synqt import appmodel

try: # tomllib is 3.11+; the CLI still supports 3.9.
    import tomllib
except ModuleNotFoundError: # pragma: no cover - only on 3.9/3.10
    tomllib = None

PROJECT = Path(__file__).resolve().parents[1]
CHECKOUT = PROJECT.parents[1]


def _pyproject():
    if tomllib is None:
        pytest.skip("tomllib needs Python 3.11 or newer")
    with (PROJECT / "pyproject.toml").open("rb") as handle:
        return tomllib.load(handle)


def _framework(root: Path) -> Path:
    """A directory that passes for a framework root, at `root`."""
    (root / "src").mkdir(parents=True)
    (root / "cmake").mkdir(parents=True)
    return root


def test_distribution_declares_the_console_script():
    assert _pyproject()["project"]["scripts"] == {"synqt": "synqt.cli:main"}


def test_version_is_read_from_the_one_module_the_release_stamps():
    config = _pyproject()
    assert config["project"]["dynamic"] == ["version"]
    assert "version" not in config["project"], (
        "a literal version here would drift from synqt/_version.py, which is the file the "
        "release workflow stamps")
    attr = config["tool"]["setuptools"]["dynamic"]["version"]["attr"]
    assert attr == "synqt._version.__version__"


def test_license_is_declared_and_its_text_ships():
    config = _pyproject()
    assert config["project"]["license"] == "Apache-2.0"
    assert config["project"]["license-files"] == ["LICENSE"]
    assert (PROJECT / "LICENSE").is_file()
    assert not any(value.startswith("License ::")
                   for value in config["project"]["classifiers"]), (
        "PEP 639 rejects a License :: classifier alongside a license expression")


def test_readme_ships_and_is_the_one_pypi_renders():
    assert _pyproject()["project"]["readme"] == "README.md"
    assert (PROJECT / "README.md").is_file()


def test_package_data_covers_the_assets_and_the_vendored_framework():
    """Every asset in the tree, not a list of patterns somebody remembered to widen.

    An asset the wheel does not carry fails where it is used and not where it was left out:
    a missing logo is a client build with no loading page, and a missing editor file is
    `synqt design` serving nothing. The vendored framework is checked by name because it
    does not exist in a checkout; the backend puts it there at build time.
    """
    patterns = _pyproject()["tool"]["setuptools"]["package-data"]["synqt"]
    assert "framework/**/*" in patterns
    package = PROJECT / "synqt"
    # Expanded the way setuptools expands it, rather than matched by hand: `**` means
    # something different to glob than it does to fnmatch, and the question here is what
    # the wheel ends up holding.
    packaged = {Path(found).resolve() for pattern in patterns
                for found in glob.glob(str(package / pattern), recursive=True)}
    assets = [path for path in sorted((package / "assets").rglob("*"))
              if path.is_file() and "__pycache__" not in path.parts]
    assert assets, "no assets at all, which is not a passing state"
    for path in assets:
        assert path.resolve() in packaged, \
            (f"{path.relative_to(package).as_posix()} matches no package-data pattern, so "
             "an installed synqt lacks it")


def test_the_build_backend_is_in_tree_and_shipped_in_the_sdist():
    config = _pyproject()
    assert config["build-system"]["build-backend"] == "_build_backend"
    assert config["build-system"]["backend-path"] == ["."]
    assert (PROJECT / "_build_backend.py").is_file()
    # Without this line pip cannot build the sdist it just downloaded: backend-path names a
    # module that is not part of the `synqt` package, so setuptools does not ship it by default.
    assert "include _build_backend.py" in (PROJECT / "MANIFEST.in").read_text()


def test_the_backend_vendors_exactly_what_the_generated_cmake_resolves():
    sys.path.insert(0, str(PROJECT))
    try:
        import _build_backend
    finally:
        sys.path.pop(0)
    for name in _build_backend._FRAMEWORK_DIRS:
        assert (CHECKOUT / name).is_dir(), f"{name}/ is not at the top of the checkout"


def test_framework_root_prefers_an_explicit_checkout(tmp_path, monkeypatch):
    named = _framework(tmp_path / "named")
    monkeypatch.setenv("SYNQT_ROOT", str(named))
    assert appmodel.framework_root() == named.resolve()


def test_framework_root_refuses_a_synqt_root_that_holds_no_framework(tmp_path, monkeypatch):
    monkeypatch.setenv("SYNQT_ROOT", str(tmp_path))
    with pytest.raises(appmodel.AppGenError) as error:
        appmodel.framework_root()
    assert "SYNQT_ROOT" in str(error.value)


def test_framework_root_finds_the_surrounding_checkout(monkeypatch):
    monkeypatch.delenv("SYNQT_ROOT", raising=False)
    assert appmodel.framework_root() == CHECKOUT


def test_framework_root_falls_back_to_the_packaged_copy(tmp_path, monkeypatch):
    """An installed wheel: no checkout above the package, a `framework/` inside it."""
    package = tmp_path / "site-packages" / "synqt"
    package.mkdir(parents=True)
    bundled = _framework(package / "framework")
    monkeypatch.delenv("SYNQT_ROOT", raising=False)
    monkeypatch.setattr(appmodel, "__file__", str(package / "appmodel.py"))
    assert appmodel.framework_root() == bundled.resolve()


def test_framework_root_refuses_a_copy_that_dies_with_the_process(tmp_path, monkeypatch):
    """A one-file frozen binary unpacks into a directory it deletes on exit, and `synqt new`
    writes the resolved root into the project's CMakeLists.txt for every later build."""
    extraction = tmp_path / "_MEI12345"
    package = extraction / "synqt"
    package.mkdir(parents=True)
    _framework(package / "framework")
    monkeypatch.delenv("SYNQT_ROOT", raising=False)
    monkeypatch.setattr(appmodel, "__file__", str(package / "appmodel.py"))
    monkeypatch.setattr(sys, "_MEIPASS", str(extraction), raising=False)
    monkeypatch.setattr(sys, "executable", str(tmp_path / "elsewhere" / "synqt"))
    with pytest.raises(appmodel.AppGenError) as error:
        appmodel.framework_root()
    assert "still exist" in str(error.value)


def test_framework_root_accepts_a_one_directory_freeze(tmp_path, monkeypatch):
    """The same check must not reject a one-directory freeze, which unpacks next to its own
    executable and stays there."""
    extraction = tmp_path / "synqt-app"
    package = extraction / "synqt"
    package.mkdir(parents=True)
    bundled = _framework(package / "framework")
    monkeypatch.delenv("SYNQT_ROOT", raising=False)
    monkeypatch.setattr(appmodel, "__file__", str(package / "appmodel.py"))
    monkeypatch.setattr(sys, "_MEIPASS", str(extraction), raising=False)
    monkeypatch.setattr(sys, "executable", str(extraction / "synqt"))
    assert appmodel.framework_root() == bundled.resolve()
