# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The scope vocabulary the build generates from `scopes.order`.

The point of the enum is that a mapping hook cannot spell a scope wrong: it returns a
member, the edge is handed that member's index, and the index is looked up in the same
list the enum was generated from. So the rules worth testing are the ones that keep the
two ends the same list. The mapping from a scope name to a member is one function, a
collision is refused rather than merged, and the values are the indices.
"""

import tempfile
import unittest
from pathlib import Path

from synqt import appgen, scopegen


class MemberNameTest(unittest.TestCase):
    def test_member_names_are_upper_camel(self):
        self.assertEqual(scopegen.member_name("admin"), "Admin")
        self.assertEqual(scopegen.member_name("power_user"), "PowerUser")
        self.assertEqual(scopegen.member_name("anonymous"), "Anonymous")

    def test_members_keep_declaration_order(self):
        order = ["anonymous", "user", "moderator", "admin"]
        self.assertEqual(scopegen.members(order), [
            ("anonymous", "Anonymous"),
            ("user", "User"),
            ("moderator", "Moderator"),
            ("admin", "Admin"),
        ])

    def test_a_collision_is_refused_rather_than_merged(self):
        # Two declared scopes sharing one member would give the hook one way to ask for
        # two different authorities, and every check downstream would agree with whichever
        # one won.
        with self.assertRaises(ValueError) as caught:
            scopegen.members(["power_user", "powerUser"])
        self.assertIn("power_user", str(caught.exception))
        self.assertIn("powerUser", str(caught.exception))

    def test_a_name_that_is_not_an_identifier_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            scopegen.members(["read:user"])
        self.assertIn("read:user", str(caught.exception))


class RenderTest(unittest.TestCase):
    def test_rendered_enum_values_are_the_indices(self):
        text = scopegen.render_scope_qml(["anonymous", "user", "admin"])
        self.assertIn("enum Value { Anonymous, User, Admin }", text)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", text)

    def test_the_comment_records_which_index_is_which_scope(self):
        # The generated file is what somebody reads when a hook returns the wrong number.
        text = scopegen.render_scope_qml(["anonymous", "user", "admin"])
        self.assertIn("//   0 = anonymous", text)
        self.assertIn("//   2 = admin", text)

    def test_there_is_no_unset_member(self):
        # Every member is a scope the project declared, which is what makes an
        # out-of-range answer from a hook a refusal rather than a fallback.
        text = scopegen.render_scope_qml(["anonymous", "user"])
        self.assertNotIn("Unset", text)

    def test_an_empty_vocabulary_is_refused_here_rather_than_by_qmlcachegen(self):
        with self.assertRaises(ValueError) as caught:
            scopegen.render_scope_qml([])
        self.assertIn("scopes.order", str(caught.exception))


class ScopeQmlPathTest(unittest.TestCase):
    def test_it_lands_beside_the_hook(self):
        # Beside, because a QML component resolves an unqualified type against its own
        # directory: next to the hook, `Scope.Value.Admin` needs no import.
        self.assertEqual(scopegen.scope_qml_path("web/edge/identity/map.qml"),
                         "web/edge/identity/Scope.qml")

    def test_a_hook_at_the_root_puts_it_at_the_root(self):
        self.assertEqual(scopegen.scope_qml_path("map.qml"), "Scope.qml")


class WrittenBesideTheHookTest(unittest.TestCase):
    """appgen writes Scope.qml, and where it writes it is the whole point."""

    def project(self, **overrides):
        root = Path(tempfile.mkdtemp())
        hook = root / "web" / "edge" / "identity" / "map.qml"
        hook.parent.mkdir(parents=True, exist_ok=True)
        hook.write_text("import SynQt\nIdentityMapping {\n"
                        "    function scopeFor(identity): int { return Scope.Value.User; }\n}\n",
                        encoding="utf-8")
        config = {
            "project": {"name": "app"},
            "entities": [
                {"name": "app", "type": "client"},
                {"name": "edge", "type": "web_edge"},
            ],
            "connect_points": [{"owner": "edge", "consumers": ["app"]}],
            "scopes": {"order": ["anonymous", "user", "moderator", "admin"]},
            "identity": {"providers": [{"name": "github"}],
                         "mapping": {"hook": "web/edge/identity/map.qml"}},
        }
        config.update(overrides)
        return root, config

    def test_it_lands_next_to_the_mirrored_hook(self):
        # Next to the *mirrored* hook under generated/, because generated/ is the tree the
        # engine loads: a QML component resolves an unqualified type against its own
        # directory, so this placement is what lets the hook say `Scope.Value.User` with
        # no import. Beside the authored hook it would be beside a file nothing runs.
        root, config = self.project()
        written = appgen.generate(root, config)
        self.assertIn("generated/web/edge/identity/Scope.qml", written)
        beside = root / "generated" / "web" / "edge" / "identity"
        self.assertTrue((beside / "Scope.qml").is_file())
        self.assertTrue((beside / "map.qml").is_file())

    def test_the_enum_is_the_projects_own_vocabulary(self):
        root, config = self.project()
        appgen.generate(root, config)
        text = (root / "generated" / "web" / "edge" / "identity"
                / "Scope.qml").read_text(encoding="utf-8")
        self.assertIn("enum Value { Anonymous, User, Moderator, Admin }", text)

    def test_a_project_with_no_hook_gets_no_file(self):
        # A project with no sign-in declares no scopes and has nothing to name, so this
        # generator must not write an enum into a tree that would never load it.
        root, config = self.project()
        del config["identity"]
        written = appgen.generate(root, config)
        self.assertEqual([path for path in written if "Scope.qml" in path], [])


if __name__ == "__main__":
    unittest.main()
