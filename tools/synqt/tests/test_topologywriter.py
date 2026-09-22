# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The per-entity topology.json writer: the shared endpoint of owner and consumer, the local
socket path, type, provider and schema pass-through (secrets as env: references), and a file
for every service entity but not the client or the edge.
"""

import json
import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import build as buildmod
from synqt import newproject, topologywriter


def _config():
    """Client -> edge -> database, plus a local-socket link to a second service."""
    return {
        "project": {"name": "shop"},
        "entities": [
            {"name": "client", "type": "client", "targets": ["wasm"]},
            {"name": "web", "type": "web_edge"},
            {"name": "database", "type": "relational",
             "provider": {"name": "postgres", "host": "db.internal", "port": 5432,
                          "database": "shop", "user": "shop",
                          "password": "env:DB_PASSWORD", "sslmode": "verify-full"}},
            {"name": "jobs", "type": "jobs"},
        ],
        "connect_points": [
            {"owner": "database", "consumers": ["web"]},
            {"owner": "web", "consumers": ["client"]},
            {"owner": "jobs", "consumers": ["database"], "transport": "local"},
        ],
    }


class ResolveEndpointsTest(unittest.TestCase):
    def test_owner_and_consumer_share_the_same_mesh_endpoint(self):
        config = _config()
        root = Path(tempfile.mkdtemp())
        endpoints = topologywriter.resolve_endpoints(config, "shop")

        database = topologywriter.entity_topology(
            config, config["entities"][2], root, endpoints)
        # The database owns its point. The web edge consumes it. Both must dial one address.
        db_items = next(cp for cp in database["connect_points"] if cp["name"] == "database")
        self.assertEqual(db_items["endpoint"]["transport"], "mtls")
        self.assertEqual(db_items["endpoint"]["host"], "127.0.0.1")
        self.assertEqual(db_items["endpoint"]["port"], endpoints["database"]["port"])
        self.assertGreaterEqual(db_items["endpoint"]["port"], topologywriter.MESH_PORT_BASE)

    def test_ports_are_deterministic_by_sorted_name(self):
        config = _config()
        first = topologywriter.resolve_endpoints(config, "shop")
        second = topologywriter.resolve_endpoints(config, "shop")
        self.assertEqual(first, second)
        # `database` sorts before `web`, so it takes the lower slot.
        self.assertLess(first["database"]["port"], first["web"]["port"])

    def test_local_transport_gets_a_socket_not_a_port(self):
        endpoints = topologywriter.resolve_endpoints(_config(), "shop")
        self.assertEqual(endpoints["jobs"]["transport"], "local")
        self.assertIn("socket", endpoints["jobs"])
        self.assertNotIn("port", endpoints["jobs"])
        self.assertEqual(endpoints["jobs"]["socket"], "synqt-shop-jobs")


class EntityTopologyTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.config = _config()
        self.endpoints = topologywriter.resolve_endpoints(self.config, "shop")

    def test_paths_are_named_from_the_project_root(self):
        # Every entity runs from the project root, and a tree built on one machine is copied
        # to another, so no path may name the build machine's directories.
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertEqual(topology["credentials"], {"ca": "synqt/mesh/ca.crt",
                                                   "cert": "synqt/mesh/database.crt",
                                                   "key": "synqt/mesh/database.key"})
        for point in topology["connect_points"]:
            self.assertFalse(Path(point["server"]).is_absolute(), point["server"])
        self.assertNotIn(self.root.resolve().as_posix(), json.dumps(topology))

    def test_type_and_provider_pass_through_with_secret_as_env_reference(self):
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertEqual(topology["type"], "relational")
        # The provider block is carried through with the secret as an env: reference.
        self.assertEqual(topology["provider"]["name"], "postgres")
        self.assertEqual(topology["provider"]["password"], "env:DB_PASSWORD")

    def test_an_embedded_database_without_a_file_gets_the_scaffolded_one(self):
        # With no file the runtime opened a temporary database and lost it on restart.
        entity = {"name": "books", "type": "relational"}
        config = dict(self.config, entities=self.config["entities"] + [entity])
        topology = topologywriter.entity_topology(config, entity, self.root, self.endpoints)
        self.assertEqual(topology["settings"], {"file": "db/relational/books/data/app.db"})
        self.assertNotIn("provider", topology)

    def test_a_named_sqlite_provider_without_a_file_gets_it_too(self):
        entity = {"name": "store", "type": "relational", "provider": {"name": "sqlite"}}
        config = dict(self.config, entities=self.config["entities"] + [entity])
        topology = topologywriter.entity_topology(config, entity, self.root, self.endpoints)
        self.assertEqual(topology["provider"],
                         {"name": "sqlite", "file": "db/relational/store/data/app.db"})

    def test_a_written_file_and_an_external_engine_are_kept(self):
        written = {"name": "books", "type": "relational",
                   "settings": {"file": "/var/lib/books/app.db", "journal_mode": "wal"}}
        config = dict(self.config, entities=self.config["entities"] + [written])
        topology = topologywriter.entity_topology(config, written, self.root, self.endpoints)
        self.assertEqual(topology["settings"],
                         {"file": "/var/lib/books/app.db", "journal_mode": "wal"})
        postgres = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertNotIn("file", postgres["provider"])
        self.assertNotIn("settings", postgres)

    def test_schema_sql_is_split_into_forward_only_steps(self):
        (self.root / "db/relational/database").mkdir(parents=True)
        (self.root / "db/relational/database" / "schema.sql").write_text(
            "-- forward-only migrations\n"
            "CREATE TABLE items (id INTEGER PRIMARY KEY);\n"
            "CREATE TABLE tags (id INTEGER PRIMARY KEY);\n")
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        self.assertEqual(len(topology["schema"]), 2)
        self.assertTrue(topology["schema"][0].startswith("CREATE TABLE items"))
        self.assertNotIn("--", topology["schema"][0])  # the comment was stripped

    def test_a_consumer_slice_lists_the_connect_point_it_consumes(self):
        # The edge slice carries `items` with the owner's endpoint.
        web = topologywriter.entity_topology(
            self.config, self.config["entities"][1], self.root, self.endpoints)
        items = next(cp for cp in web["connect_points"] if cp["name"] == "database")
        self.assertEqual(items["owner"], "database")
        self.assertEqual(items["endpoint"]["port"], self.endpoints["database"]["port"])

    def test_server_file_is_the_owners_source_qml(self):
        topology = topologywriter.entity_topology(
            self.config, self.config["entities"][2], self.root, self.endpoints)
        items = next(cp for cp in topology["connect_points"] if cp["name"] == "database")
        self.assertTrue(items["server"].endswith("db/relational/database/Database.qml"))


class WriteTest(unittest.TestCase):
    def test_write_emits_a_file_per_service_and_for_a_mesh_consuming_edge(self):
        root = Path(tempfile.mkdtemp())
        written = topologywriter.write(root, _config())
        self.assertIn("build/database/topology.json", written)
        self.assertIn("build/jobs/topology.json", written)
        # The client always reads its config from the served page, never a topology.
        self.assertNotIn("build/client/topology.json", written)
        # The edge gets a topology for its mesh side; WebEdge keeps the browser side.
        self.assertIn("build/web/topology.json", written)
        # The emitted file parses and names its entity.
        emitted = json.loads((root / "build" / "database" / "topology.json").read_text())
        self.assertEqual(emitted["entity"], "database")

    def test_edge_topology_is_its_mesh_consumed_side_only(self):
        # Only `items`: EntityRuntime must not host `todo`, which WebEdge hosts.
        root = Path(tempfile.mkdtemp())
        topologywriter.write(root, _config())
        edge = json.loads((root / "build" / "web" / "topology.json").read_text())
        names = sorted(cp["name"] for cp in edge["connect_points"])
        self.assertEqual(names, ["database"])
        self.assertEqual(edge["entity"], "web")

    def test_an_edge_with_no_mesh_side_gets_no_topology(self):
        # An edge that consumes nothing over the mesh gets no topology.
        config = _config()
        config["connect_points"] = [
            {"owner": "web", "consumers": ["client"]}]
        root = Path(tempfile.mkdtemp())
        written = topologywriter.write(root, config)
        self.assertNotIn("build/web/topology.json", written)

    def test_build_writes_topology_for_a_typed_entity(self):
        parent = Path(tempfile.mkdtemp())
        newproject.scaffold(parent, "app", starting=[("orders", "relational")])
        root = parent / "app"
        # Wire the edge to the relational entity so there is a real mesh link.
        config = yaml.safe_load((root / "synqt.yaml").read_text())
        # The fixture declares what crosses `items`, so the project builds and the topology
        # is written.
        config["connect_points"] = [
            {"owner": "orders", "consumers": ["edge"],
             "export": "prop int count\n"}]
        (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        owner = root / "db" / "relational" / "orders"
        owner.mkdir(parents=True, exist_ok=True)

        buildmod.build(root, profile_name="release", client="wasm")
        topology_path = root / "build" / "orders" / "topology.json"
        self.assertTrue(topology_path.exists())
        topology = json.loads(topology_path.read_text())
        self.assertEqual(topology["type"], "relational")
        self.assertEqual(topology["connect_points"][0]["name"], "orders")


if __name__ == "__main__":
    unittest.main()
