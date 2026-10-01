# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``tools/wasm-shell.py`` runs on a bare interpreter, with nothing pip-installed, as the
browser harness runners do. PyYAML is denied to the interpreter, since it is installed
wherever these tests usually run.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SCRIPT = _REPO_ROOT / "tools" / "wasm-shell.py"

# sitecustomize runs before user code, so the blocker is in place for the script's imports.
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

        # Stand in for the one built file the script checks for.
        out = work / "build"
        out.mkdir()
        (out / "spike-client.js").write_text("// built client\n", encoding="utf-8")

        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(blocker)
        result = subprocess.run(
            [sys.executable, str(_SCRIPT), "--target", "spike-client", "--out", str(out)],
            capture_output=True, text=True, env=environment, cwd=str(_REPO_ROOT))

        self.assertEqual(result.returncode, 0, result.stderr)
        page = (out / "index.html").read_text(encoding="utf-8")
        self.assertIn("<title>", page)
        self.assertIn("spike-client.js", page)
        boot = (out / "synqt-boot.js").read_text(encoding="utf-8")
        self.assertIn("spike-client.wasm", boot)
        self.assertIn("window.spike_client_entry", boot)

    def test_the_blocker_really_denies_pyyaml(self):
        # The blocker itself works.
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
