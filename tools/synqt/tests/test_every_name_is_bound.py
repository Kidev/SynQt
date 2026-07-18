# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Every name a tooling module reads resolves to something that module binds.

Each module is read through `symtable`: a global read by a function must be bound at module
level (assigned, imported, defined, or declared `global`) or be a builtin. Anything else is
a missing import or a typo on an untested branch. Standard library only.
"""

import builtins
import symtable
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]

MODULES = sorted(
    [*REPO.joinpath("tools", "synqt", "synqt").glob("*.py"),
     *REPO.joinpath("tools", "synqtc", "synqtc").glob("*.py"),
     *REPO.joinpath("tools").glob("*.py"),
     *REPO.joinpath("tests").glob("*.py")])

# Module names besides the builtins, including symbol-table additions (Python 3.14 records
# `__conditional_annotations__`).
_MODULE_NAMES = {"__file__", "__name__", "__doc__", "__spec__", "__package__", "__path__",
                 "__loader__", "__builtins__", "__annotations__", "__dict__",
                 "__conditional_annotations__", "__classdict__", "__classdictcell__"}


def _tables(table):
    yield table
    for child in table.get_children():
        yield from _tables(child)


def unbound_names(source: str) -> list:
    """Every global name read in `source` that its module never binds, as `scope:name`."""
    top = symtable.symtable(source, "<module>", "exec")
    bound = set(dir(builtins)) | _MODULE_NAMES
    for symbol in top.get_symbols():
        if symbol.is_assigned() or symbol.is_imported() or symbol.is_namespace():
            bound.add(symbol.get_name())
    # A function that says `global x` and assigns it binds a module name too.
    for table in _tables(top):
        for symbol in table.get_symbols():
            if symbol.is_declared_global() and symbol.is_assigned():
                bound.add(symbol.get_name())
    found = set()
    for table in _tables(top):
        for symbol in table.get_symbols():
            if not symbol.is_referenced() or symbol.get_name() in bound:
                continue
            at_module = table is top and not symbol.is_assigned()
            if at_module or (table is not top and symbol.is_global()):
                found.add(f"{table.get_name()}:{symbol.get_name()}")
    return sorted(found)


def test_the_check_sees_a_missing_import():
    # A module called by name and never imported.
    assert unbound_names("def dev(root):\n    mesh.ensure_gitignored(root)\n") == [
        "dev:mesh"]


def test_the_check_sees_a_name_bound_only_in_another_function():
    # The exact shape of the run.py crash: imported locally in one function, read in another.
    source = ("def status():\n    from . import mesh\n    return mesh\n"
              "def dev(root):\n    mesh.ensure_gitignored(root)\n")
    assert unbound_names(source) == ["dev:mesh"]


def test_the_check_is_quiet_about_what_is_bound():
    source = ("import os.path\nfrom . import mesh as m\n"
              "def f(a, *rest, key=None, **more):\n"
              "    total = [x for x in rest]\n"
              "    try:\n        pass\n    except OSError as error:\n        print(error)\n"
              "    return os.path, m, a, total, key, more, len\n")
    assert unbound_names(source) == []


@pytest.mark.parametrize("module", MODULES, ids=lambda path: str(path.relative_to(REPO)))
def test_every_name_a_module_reads_is_bound_in_it(module):
    assert unbound_names(module.read_text(encoding="utf-8")) == [], module
