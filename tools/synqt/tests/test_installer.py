# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""get.synqt.org serves one installer under two names (`/` and `/install.sh`), so index.html
must equal install.sh. The release workflow checks it too. Both installers refuse an asset
whose digest is not the one the release's SHA256SUMS lists.
"""

import hashlib
import os
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

CHECKOUT = Path(__file__).resolve().parents[3]
SITE = CHECKOUT / "deploy" / "get.synqt.org"


def _read(name: str) -> str:
    path = SITE / name
    if not path.is_file():
        pytest.skip(f"{path} is not in this tree (running outside a checkout)")
    return path.read_text(encoding="utf-8")


def test_the_index_is_a_copy_of_the_installer():
    assert _read("index.html") == _read("install.sh"), (
        "deploy/get.synqt.org/index.html must be a byte for byte copy of install.sh "
        "(cp deploy/get.synqt.org/install.sh deploy/get.synqt.org/index.html)")


# The installer run against a fake release: `curl` and `uname` on PATH are stand-ins that
# serve files from a directory, so the script runs unchanged and reaches no network.
FAKE_CURL = """#!/bin/sh
out=""
url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    -*) shift ;;
    *) url="$1"; shift ;;
  esac
done
src="$RELEASE_DIR/${url##*/}"
[ -f "$src" ] || exit 22
cp "$src" "$out"
"""

FAKE_UNAME = """#!/bin/sh
case "$1" in
  -s) echo Linux ;;
  -m) echo x86_64 ;;
esac
"""

ASSET = "synqt-linux-x86_64.tar.gz"

posix_only = pytest.mark.skipif(
    shutil.which("sh") is None or shutil.which("tar") is None, reason="needs sh and tar")


def _release(tmp_path, sums):
    """A release directory holding the asset and, unless `sums` is None, a SHA256SUMS
    built by `sums(digest)` from the asset's real digest."""
    release = tmp_path / "release"
    payload = tmp_path / "payload"
    release.mkdir()
    payload.mkdir()
    binary = payload / "synqt"
    binary.write_text("#!/bin/sh\necho synqt\n", encoding="utf-8")
    with tarfile.open(release / ASSET, "w:gz") as archive:
        archive.add(binary, arcname="synqt")
    digest = hashlib.sha256((release / ASSET).read_bytes()).hexdigest()
    if sums is not None:
        (release / "SHA256SUMS").write_text(sums(digest), encoding="utf-8")
    return release


def _install(tmp_path, release):
    fakes = tmp_path / "fakes"
    fakes.mkdir()
    for name, body in (("curl", FAKE_CURL), ("uname", FAKE_UNAME)):
        path = fakes / name
        path.write_text(body, encoding="utf-8")
        path.chmod(0o755)
    bindir = tmp_path / "bin"
    env = dict(os.environ, PATH=f"{fakes}{os.pathsep}{os.environ['PATH']}",
               RELEASE_DIR=str(release), SYNQT_BIN=str(bindir), HOME=str(tmp_path))
    result = subprocess.run(["sh", str(SITE / "install.sh")], env=env,
                            capture_output=True, text=True, check=False)
    return result, bindir / "synqt"


@posix_only
def test_an_asset_matching_its_checksum_is_installed(tmp_path):
    release = _release(tmp_path, lambda digest: f"{'0' * 64}  other.zip\n{digest}  {ASSET}\n")
    result, installed = _install(tmp_path, release)
    assert result.returncode == 0, result.stderr
    assert installed.is_file()


@posix_only
@pytest.mark.parametrize("sums", [
    pytest.param(lambda digest: f"{'0' * 64}  {ASSET}\n", id="a-different-digest"),
    pytest.param(lambda digest: f"{digest}  synqt-macos-arm64.tar.gz\n", id="no-line-for-the-asset"),
    pytest.param(None, id="no-checksum-file"),
])
def test_an_asset_that_does_not_match_its_checksum_is_refused(tmp_path, sums):
    release = _release(tmp_path, sums)
    result, installed = _install(tmp_path, release)
    assert result.returncode != 0
    assert not installed.exists()


def test_the_windows_installer_checks_the_same_checksum_file():
    # No PowerShell on the Linux runners, so this reads the script instead of running it.
    script = _read("install.ps1")
    assert "SHA256SUMS" in script
    assert "Get-FileHash" in script
