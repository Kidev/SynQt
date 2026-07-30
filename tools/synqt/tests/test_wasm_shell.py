# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``tools/wasm-shell.py`` runs on a bare interpreter, with nothing pip-installed.

Every raw WebAssembly spike under ``tests/`` calls that script to get the same loading
page the product ships, and the browser harnesses that drive them run on a plain
``actions/setup-python`` runner: no ``pip install``, no virtualenv, only the standard
library. So the script's dependency set is part of its contract, and this is the test
that keeps it honest.

It is not a hypothetical. A ``from . import addauth`` inside ``appmodel`` (for the OAuth
provider table) put PyYAML underneath ``synqt.clientshell``, and four browser jobs began
failing with "could not render the SynQt loading shell", pointing at a renderer that was
fine. Asserting on the import graph would not have caught it either, since PyYAML is
installed wherever these tests usually run; the interpreter has to be denied the module.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SCRIPT = _REPO_ROOT / "tools" / "wasm-shell.py"

# Python imports sitecustomize before any user code, so a blocker installed there is in
# place for the script's own imports. Denying the module (rather than removing it from
# the environment) is what makes this runnable on a developer machine that has PyYAML.
_SITECUSTOMIZE = """\
import sys
from importlib.abc import MetaPathFinder


class _Denied(MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "yaml" or fullname.startswith("yaml."):
            raise ModuleNotFoundError("No module named 'yaml'", name=fullname)
        return None


sys.meta_path.insert(0, _Denied())
"""


class WasmShellTest(unittest.TestCase):
    def test_renders_the_shell_without_pyyaml(self):
        work = Path(tempfile.mkdtemp())
        blocker = work / "blocker"
        blocker.mkdir()
        (blocker / "sitecustomize.py").write_text(_SITECUSTOMIZE, encoding="utf-8")

        # The script refuses to write a shell over a directory with no built client, so
        # stand in for the one file it checks for.
        out = work / "build"
        out.mkdir()
        (out / "m0-client.js").write_text("// built client\n", encoding="utf-8")

        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(blocker)
        result = subprocess.run(
            [sys.executable, str(_SCRIPT), "--target", "m0-client", "--out", str(out)],
            capture_output=True, text=True, env=environment, cwd=str(_REPO_ROOT))

        self.assertEqual(result.returncode, 0, result.stderr)
        page = (out / "index.html").read_text(encoding="utf-8")
        self.assertIn("<title>", page)
        self.assertIn("m0-client.js", page)
        boot = (out / "synqt-boot.js").read_text(encoding="utf-8")
        self.assertIn("m0-client.wasm", boot)
        self.assertIn("window.m0_client_entry", boot)

    def test_the_blocker_really_denies_pyyaml(self):
        # Without this, a broken blocker would turn the test above into a test of
        # nothing at all, and it would keep passing after PyYAML crept back in.
        work = Path(tempfile.mkdtemp())
        (work / "sitecustomize.py").write_text(_SITECUSTOMIZE, encoding="utf-8")

        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(work)
        result = subprocess.run([sys.executable, "-c", "import yaml"],
                                capture_output=True, text=True, env=environment)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No module named 'yaml'", result.stderr)


if __name__ == "__main__":
    unittest.main()
