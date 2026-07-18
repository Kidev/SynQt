# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The per-entity topology.json writer: the machine form the service runtime reads.

Covers the shared-endpoint invariant (owner and consumer agree on host+port), the
local-socket path, blueprint/provider/schema pass-through (with secrets kept as env:
references), and that the writer emits a file for every service entity but not the client
or the edge.
"""

import json
import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import build as buildmod
from synqt import newproject, topologywriter


def _config():
    """A three-entity system: client -> edge -> database, plus a local-socket link to a
    second service, to exercise both mesh transports."""
    return {
        "project": {"name": "shop"},
        "entities": [
            {"name": "client", "kind": "client", "targets": ["wasm"]},
            {"name": "web", "kind": "service", "capability": "web_edge"},
            {"name": "database", "kind": "service", "blueprint": "persistence",
             "provider": {"name": "postgres", "host": "db.internal", "port": 5432,
                          "database": "shop", "user": "shop",
                          "password": "env:DB_PASSWORD", "sslmode": "verify-full"}},
            {"name": "jobs", "kind": "service", "blueprint": "jobs"},
        ],
        "connect_points": [
            {"name": "items", "owner": "database", "contract": "Items",
             "consumers": ["web"]},
            {"name": "todo", "owner": "web", "contract": "Todo",
             "consumers": ["client"]},
            {"name": "rollup", "owner": "jobs", "contract": "Rollup",
             "consumers": ["database"], "transport": "local"},
        ],
    }


class ResolveEndpointsTest(unittest.TestCase):
    def test_owner_and_consumer_share_the_same_mesh_endpoint(self):
        config = _config()
        root = Path(tempfile.mkdtemp())
        endpoints = topologywriter.resolve_endpoints(config, "shop")

        database = topologywriter.entity_topology(
            config, config["entities"][2], root, endpoints)
        # The database owns `items`; the web edge consumes it; both must dial one address.
        db_items = next(cp for cp in database["connect_points"] if cp["name"] == "items")
        self.assertEqual(db_items["endpoint"]["transport"], "mtls")
        self.assertEqual(db_items["endpoint"]["host"], "127.0.0.1")
        self.assertEqual(db_items["endpoint"]["port"], endpoints["items"]["port"])
        self.assertGreaterEqual(db_items["endpoint"]["port"], topologywriter.MESH_PORT_BASE)

    def test_ports_are_deterministic_by_sorted_name(self):
        config = _config()
        first = topologywriter.resolve_endpoints(config, "shop")
        second = topologywriter.resolve_endpoints(config, "shop")
        self.assertEqual(first, second)
        # `items` sorts before `todo`, so it takes the lower slot.
        self.assertLess(first["items"]["port"], first["todo"]["port"])

    def test_local_transport_gets_a_socket_not_a_port(self):
        endpoints = topologywriter.resolve_endpoints(_config(), "shop")
        self.assertEqual(endpoints["rollup"]["transport"], "local")
        self.assertIn("socket", endpoints["rollup"])
        self.assertNotIn("port", endpoints["rollup"])
        self.assertEqual(endpoints["rollup"]["socket"], "synqt-shop-rollup")


class EntityTopologyTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.config = _config()
        self.endpoints = topologywriter.resolve_endpoints(self.config, "shop")

    def test_credentials_point_at_the_entitys_mesh_material_absolutely(self):
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        creds = topology["credentials"]
        self.assertTrue(creds["cert"].endswith("synqt/mesh/database.crt"))
        self.assertTrue(creds["key"].endswith("synqt/mesh/database.key"))
        self.assertTrue(creds["ca"].endswith("synqt/mesh/ca.crt"))
        self.assertTrue(Path(creds["cert"]).is_absolute())

    def test_blueprint_and_provider_pass_through_with_secret_as_env_reference(self):
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertEqual(topology["blueprint"], "persistence")
        # The provider block is carried through; the secret stays an env: reference, never
        # resolved into the file.
        self.assertEqual(topology["provider"]["name"], "postgres")
        self.assertEqual(topology["provider"]["password"], "env:DB_PASSWORD")

    def test_schema_sql_is_split_into_forward_only_steps(self):
        (self.root / "database").mkdir(parents=True)
        (self.root / "database" / "schema.sql").write_text(
            "-- forward-only migrations\n"
            "CREATE TABLE items (id INTEGER PRIMARY KEY);\n"
            "CREATE TABLE tags (id INTEGER PRIMARY KEY);\n")
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertEqual(len(topology["schema"]), 2)
        self.assertTrue(topology["schema"][0].startswith("CREATE TABLE items"))
        self.assertNotIn("--", topology["schema"][0])  # the comment was stripped

    def test_a_consumer_slice_lists_the_connect_point_it_consumes(self):
        # The web edge consumes `items`; its slice must carry that connect point (deny by
        # default is derived from this list) with the same endpoint the owner listens on.
        web = topologywriter.entity_topology(
            self.config, self.config["entities"][1], self.root, self.endpoints)
        items = next(cp for cp in web["connect_points"] if cp["name"] == "items")
        self.assertEqual(items["owner"], "database")
        self.assertEqual(items["endpoint"]["port"], self.endpoints["items"]["port"])

    def test_server_file_is_the_owners_source_qml(self):
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        items = next(cp for cp in topology["connect_points"] if cp["name"] == "items")
        self.assertTrue(items["server"].endswith("database/Items.qml"))


class WriteTest(unittest.TestCase):
    def test_write_emits_a_file_per_service_and_for_a_mesh_consuming_edge(self):
        root = Path(tempfile.mkdtemp())
        written = topologywriter.write(root, _config())
        self.assertIn("build/database/topology.json", written)
        self.assertIn("build/jobs/topology.json", written)
        # The client always reads its config from the served page, never a topology.
        self.assertNotIn("build/client/topology.json", written)
        # The edge reaches the database over the mesh (it consumes `items`), so it now gets
        # a topology for that mesh side; its browser-facing side stays with WebEdge.
        self.assertIn("build/web/topology.json", written)
        # The emitted file parses and names its entity.
        emitted = json.loads((root / "build" / "database" / "topology.json").read_text())
        self.assertEqual(emitted["entity"], "database")

    def test_edge_topology_is_its_mesh_consumed_side_only(self):
        # The edge owns `todo` (browser-facing, hosted by WebEdge) and consumes `items` over
        # the mesh. Its topology must list only `items`, or EntityRuntime would try to host
        # `todo` too and collide with WebEdge.
        root = Path(tempfile.mkdtemp())
        topologywriter.write(root, _config())
        edge = json.loads((root / "build" / "web" / "topology.json").read_text())
        names = sorted(cp["name"] for cp in edge["connect_points"])
        self.assertEqual(names, ["items"])
        self.assertEqual(edge["entity"], "web")

    def test_an_edge_with_no_mesh_side_gets_no_topology(self):
        # An edge that consumes nothing over the mesh (only owns browser-facing points) needs
        # no EntityRuntime and thus no topology.
        config = _config()
        config["connect_points"] = [
            {"name": "todo", "owner": "web", "contract": "Todo", "consumers": ["client"]}]
        root = Path(tempfile.mkdtemp())
        written = topologywriter.write(root, config)
        self.assertNotIn("build/web/topology.json", written)

    def test_build_writes_topology_for_a_blueprint_entity(self):
        parent = Path(tempfile.mkdtemp())
        newproject.scaffold(parent, "app", blueprints=["persistence"])
        root = parent / "app"
        # Wire the edge to the persistence entity so there is a real mesh link.
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        config["connect_points"] = [
            {"name": "items", "owner": "persistence", "contract": "Items",
             "consumers": ["web"]}]
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        # The declared contract has to exist on disk. Declaring `items` without writing
        # shared/Items.syn leaves a project that cannot configure, and this test used to pass
        # anyway: build() caught the CMake failure, returned it as a note, and carried on
        # writing the topology this asserts on. It raises now, so the fixture has to be a
        # project that really builds -- which is the only version of it that proves anything.
        (root / "shared").mkdir(exist_ok=True)
        (root / "shared" / "Items.syn").write_text(
            "// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
            "// SPDX-License-Identifier: Apache-2.0\n"
            "\n"
            "contract Items {\n"
            "    prop int count\n"
            "}\n")

        buildmod.build(root, release=True, client="wasm")
        topology_path = root / "build" / "persistence" / "topology.json"
        self.assertTrue(topology_path.exists())
        topology = json.loads(topology_path.read_text())
        self.assertEqual(topology["blueprint"], "persistence")
        self.assertEqual(topology["connect_points"][0]["name"], "items")


if __name__ == "__main__":
    unittest.main()
