# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The scope vocabulary the build generates from `scopes.order`: one name-to-member mapping,
collisions refused, and each member's value is its index.
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
        # Two scopes on one member would give the hook one answer for two authorities.
        with self.assertRaises(ValueError) as caught:
            scopegen.members(["power_user", "powerUser"])
        self.assertIn("power_user", str(caught.exception))
        self.assertIn("powerUser", str(caught.exception))

    def test_a_scope_that_collides_with_the_enums_own_name_is_refused(self):
        # A scope called `value` would make `Scope.Value` both the enum and a member; QML
        # picks the enum and the login fails closed.
        with self.assertRaises(ValueError) as caught:
            scopegen.members(["anonymous", "value"])
        self.assertIn("value", str(caught.exception))

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
        # No `Unset` member, so an out-of-range hook answer is refused.
        text = scopegen.render_scope_qml(["anonymous", "user"])
        self.assertNotIn("Unset", text)

    def test_an_empty_vocabulary_is_refused_here_rather_than_by_qmlcachegen(self):
        with self.assertRaises(ValueError) as caught:
            scopegen.render_scope_qml([])
        self.assertIn("scopes.order", str(caught.exception))


class ScopeQmlPathTest(unittest.TestCase):
    def test_it_lands_beside_the_hook(self):
        # Beside the hook, so `Scope.Admin` needs no import.
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
                        "    function scopeFor(identity): int { return Scope.User; }\n}\n",
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
        # Beside the mirrored hook under generated/, the tree the engine loads.
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
        # No sign-in, no scopes, no enum.
        root, config = self.project()
        del config["identity"]
        written = appgen.generate(root, config)
        self.assertEqual([path for path in written if "Scope.qml" in path], [])


if __name__ == "__main__":
    unittest.main()
