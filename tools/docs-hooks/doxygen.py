# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that generates the C++ runtime reference into the built site.

Doxygen renders the classes in `src/` into `<site>/api/`, which the "C++ API reference"
page links to. The Doxyfile at the repository root holds every setting; only the output
location is overridden here, so `doxygen Doxyfile` on its own produces the same pages in
`build/apidocs/`.

Doxygen is optional. Without it the rest of the site still builds, and the hook says so
rather than failing: only the API reference pages are missing. Continuous integration
installs it (see .github/workflows/docs.yml), so the published site always has them.
"""

import logging
import shutil
import subprocess
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.doxygen")

_ROOT = Path(__file__).resolve().parents[2]
_DOXYFILE = _ROOT / "Doxyfile"


def on_post_build(config, **kwargs):
    doxygen = shutil.which("doxygen")
    if doxygen is None:
        log.warning("doxygen not found: skipping the C++ API reference (/api/)")
        return
    if not _DOXYFILE.is_file():
        log.warning("no Doxyfile at %s: skipping the C++ API reference", _DOXYFILE)
        return

    site_dir = Path(config["site_dir"])
    overrides = "\n".join([
        _DOXYFILE.read_text(encoding="utf-8"),
        "OUTPUT_DIRECTORY = %s" % site_dir,
        "HTML_OUTPUT = api",
        "",
    ])

    result = subprocess.run([doxygen, "-"], input=overrides, text=True, cwd=_ROOT,
                            capture_output=True)
    if result.returncode != 0:
        log.warning("doxygen failed (%d), the C++ API reference is missing:\n%s",
                    result.returncode, result.stderr.strip())
        return
    for line in result.stderr.splitlines():
        if line.strip():
            log.warning("doxygen: %s", line.strip())
    log.info("C++ API reference generated into %s", site_dir / "api")
