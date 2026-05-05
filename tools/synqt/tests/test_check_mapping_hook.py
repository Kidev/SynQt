# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The mapping hook may only name scopes the project declared.

The edge bounds-checks the hook's answer at login (identityprovider.cpp, mapScope), which
catches a typo the first time somebody signs in. This catches it while the project is being
written, which is where the author asked for it and where the fix is one character.
"""

import tempfile
import unittest
from pathlib import Path

from synqt import check

PROJECT = """project:
  name: app

scopes:
  order: [anonymous, user, moderator, admin]

entities:
  - name: app
    type: client
  - name: edge
    type: web_edge

connect_points:
  - owner: edge
    consumers: [app]
    export: |
      prop int tick

identity:
  providers:
    - name: github
      client_id: abc
      client_secret: env:GITHUB_CLIENT_SECRET
  mapping:
    hook: web/edge/identity/map.qml
"""

HOOK = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

IdentityMapping {
    function scopeFor(identity): int {
        return Scope.Value.%s;
    }
}
"""


def project(member="Admin", source=PROJECT, hook=None):
    """A project on disk whose edge holds the mapping hook its identity names."""
    root = Path(tempfile.mkdtemp())
    (root / "synqt.yaml").write_text(source, encoding="utf-8")
    (root / "client" / "app").mkdir(parents=True)
    (root / "client" / "app" / "Main.qml").write_text(
        "import QtQuick\nimport QtQuick.Window\n\nWindow { visible: true }\n",
        encoding="utf-8")
    (root / "web" / "edge").mkdir(parents=True)
    (root / "web" / "edge" / "Edge.qml").write_text(
        "import SynQt\n\nEdge {\n    tick: 0\n}\n", encoding="utf-8")
    hook_path = root / "web" / "edge" / "identity" / "map.qml"
    hook_path.parent.mkdir(parents=True)
    hook_path.write_text(HOOK % member if hook is None else hook, encoding="utf-8")
    return root


def errors(root):
    ok, messages = check.check_project(root)
    return ok, [m for m in messages if m.startswith("error:")]


class MappingHookMembersTest(unittest.TestCase):
    def test_a_hook_naming_an_undeclared_member_is_refused(self):
        ok, found = errors(project(member="Admn"))
        self.assertFalse(ok, found)
        self.assertTrue(any("Admn" in m and "Admin" in m for m in found), found)

    def test_a_hook_naming_declared_members_is_accepted(self):
        # A checker tested only by refusals passes when it refuses everything.
        ok, found = errors(project(member="Admin"))
        self.assertTrue(ok, found)

    def test_every_member_in_the_file_is_checked_not_just_the_first(self):
        hook = HOOK % "Admin"
        hook = hook.replace("        return Scope.Value.Admin;\n",
                            "        if (identity.email) {\n"
                            "            return Scope.Value.Moderator;\n"
                            "        }\n"
                            "        return Scope.Value.Superuser;\n")
        ok, found = errors(project(hook=hook))
        self.assertFalse(ok, found)
        self.assertTrue(any("Superuser" in m for m in found), found)

    def test_a_member_named_in_a_comment_is_not_a_reference(self):
        # Tokenized rather than pattern-matched. Refusing a name written in a comment would
        # make a comment fail a build, and the hook's comments are where the scopes get
        # explained.
        hook = HOOK % "Admin"
        hook = hook.replace("IdentityMapping {",
                            "// Return Scope.Value.Superuser for nobody; it does not exist.\n"
                            "IdentityMapping {")
        ok, found = errors(project(hook=hook))
        self.assertTrue(ok, found)

    def test_a_hook_the_project_names_but_does_not_have_is_refused(self):
        root = project()
        (root / "web" / "edge" / "identity" / "map.qml").unlink()
        ok, found = errors(root)
        self.assertFalse(ok, found)
        self.assertTrue(any("map.qml" in m for m in found), found)


if __name__ == "__main__":
    unittest.main()
