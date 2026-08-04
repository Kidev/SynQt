# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What `synqt docker` generates, and the properties that make it more than a template.

The image build itself is not exercised here (it downloads a Qt kit); what is asserted is
everything that decides whether that build produces a system that comes up: the addresses
the entities dial each other on, the one entity allowed a published port, what is kept out
of the build context, and the arrangement that lets an entity reach its engine without a
password crossing a network.
"""

import unittest
from pathlib import Path

import yaml

from synqt import docker


def _config(**overrides):
    """A two-entity project: a client, a web edge, and a persistence entity."""
    config = {
        "project": {"name": "shop"},
        "entities": [
            {"name": "client", "kind": "client", "edge": "web"},
            {"name": "web", "kind": "service", "capability": "web_edge",
             "public": {"port": 8443}},
            {"name": "store", "kind": "service", "blueprint": "persistence"},
        ],
        "connect_points": [
            {"name": "feed", "contract": "Feed", "owner": "web", "consumers": ["client"]},
            {"name": "rows", "contract": "Rows", "owner": "store", "consumers": ["web"]},
        ],
    }
    config.update(overrides)
    return config


def _with_engine(engine="postgres", secret="DB_PASSWORD"):
    config = _config()
    for entity in config["entities"]:
        if entity["name"] == "store":
            entity["provider"] = {"name": engine, "host": "db.internal", "port": 5432,
                                  "database": "store", "user": "store",
                                  "password": f"env:{secret}", "sslmode": "verify-full"}
    return config


class AddressTest(unittest.TestCase):
    def test_one_address_per_entity_and_none_for_the_client(self):
        addresses = docker.mesh_addresses(_config())
        self.assertEqual(sorted(addresses), ["store", "web"])

    def test_addresses_are_stable_across_runs(self):
        # They end up in the profile, in the compose file and, through the topology, in what
        # each entity dials. A regenerate that renumbered them would leave a half-regenerated
        # project talking to itself wrong.
        self.assertEqual(docker.mesh_addresses(_config()),
                         docker.mesh_addresses(_config()))

    def test_addresses_start_clear_of_the_gateway(self):
        first = docker.mesh_addresses(_config())["web"]
        self.assertTrue(first.endswith(f".{docker.FIRST_HOST}"), first)

    def test_a_subnet_too_small_is_refused_with_the_numbers(self):
        config = _config()
        config["entities"] += [{"name": f"svc{index}", "kind": "service"}
                               for index in range(8)]
        with self.assertRaises(docker.DockerError) as error:
            docker.mesh_addresses(config, "10.9.9.0/29")
        self.assertIn("--subnet", str(error.exception))

    def test_a_malformed_subnet_is_refused(self):
        with self.assertRaises(docker.DockerError):
            docker.mesh_addresses(_config(), "not-a-subnet")


class ProfileTest(unittest.TestCase):
    def _profile(self, config):
        return yaml.safe_load(
            docker.render_profile(config, docker.mesh_addresses(config)))

    def test_every_entity_gets_the_address_the_compose_file_assigns(self):
        config = _config()
        addresses = docker.mesh_addresses(config)
        profile = self._profile(config)
        for entity in profile["entities"]:
            self.assertEqual(entity["mesh"]["host"], addresses[entity["name"]])

    def test_the_mesh_host_is_an_address_and_never_a_service_name(self):
        # A mesh endpoint is read into a QHostAddress (src/service/entityruntime.cpp), which
        # holds an address and not a name. A compose service name here would parse to a null
        # QHostAddress and the link would silently never connect.
        import ipaddress

        for entity in self._profile(_config())["entities"]:
            ipaddress.ip_address(entity["mesh"]["host"])

    def test_the_profile_only_changes_what_containers_change(self):
        # A profile changes and adds, never removes (config.merge). Anything else in here
        # would be a second copy of the topology, drifting from the first.
        profile = self._profile(_config())
        self.assertEqual(set(profile), {"entities"})
        for entity in profile["entities"]:
            self.assertTrue(set(entity) - {"name"} <= {"mesh", "tls", "provider"},
                            entity)

    def test_the_edge_gets_a_certificate_it_will_actually_have(self):
        # A scaffolded synqt.yaml points `tls:` at a deployment certificate nobody has
        # obtained yet. Left alone, the edge comes up, reports itself listening, and every
        # request fails in the handshake, which reads like a networking problem.
        profile = self._profile(_config())
        web = next(e for e in profile["entities"] if e["name"] == "web")
        self.assertEqual(web["tls"]["cert_file"], docker.EDGE_CERT)
        self.assertEqual(web["tls"]["key_file"], docker.EDGE_KEY)

    def test_only_the_edge_gets_a_browser_certificate(self):
        profile = self._profile(_config())
        store = next(e for e in profile["entities"] if e["name"] == "store")
        self.assertNotIn("tls", store)

    def test_an_engine_backed_entity_is_pointed_at_loopback(self):
        # Not at the engine's service name: the engine shares this entity's network
        # namespace, so the link genuinely is loopback. That is what an external provider
        # requires to accept plaintext in release (ProviderConfig::isLoopbackHost), and the
        # point is that nothing had to be relaxed to satisfy it.
        profile = self._profile(_with_engine())
        store = next(e for e in profile["entities"] if e["name"] == "store")
        self.assertEqual(store["provider"]["host"], "127.0.0.1")
        self.assertEqual(store["provider"]["sslmode"], "disable")


class ComposeTest(unittest.TestCase):
    def _compose(self, config, **kwargs):
        return yaml.safe_load(
            docker.render_compose(config, docker.mesh_addresses(config), **kwargs))

    def test_only_the_web_edge_publishes_a_port(self):
        # The deny-by-default topology, said in compose: everything else is reachable only
        # from inside the container network.
        services = self._compose(_config())["services"]
        published = [name for name, service in services.items() if service.get("ports")]
        self.assertEqual(published, ["web"])

    def test_the_edge_publishes_the_port_its_config_declares(self):
        compose = self._compose(_config())
        self.assertEqual(compose["services"]["web"]["ports"], ["8443:8443"])

    def test_the_port_can_be_overridden(self):
        compose = self._compose(_config(), port=9999)
        self.assertEqual(compose["services"]["web"]["ports"], ["9999:9999"])

    def test_the_client_gets_no_container(self):
        # It is a bundle the edge serves, not a process, however it was built.
        self.assertNotIn("client", self._compose(_config())["services"])

    def test_every_entity_waits_for_its_certificate(self):
        services = self._compose(_config())["services"]
        for name in ("web", "store"):
            self.assertEqual(
                services[name]["depends_on"]["mesh-init"]["condition"],
                "service_completed_successfully", name)

    def test_the_mesh_volume_is_shared_and_the_ca_is_not_in_the_image(self):
        compose = self._compose(_config())
        self.assertIn("mesh", compose["volumes"])
        for name in ("web", "store", "mesh-init"):
            mounts = compose["services"][name]["volumes"]
            self.assertTrue(any(mount.startswith("mesh:") for mount in mounts), name)

    def test_an_embedded_database_lives_in_a_volume(self):
        # Otherwise it is in the container's own layer, and `up --build` starts the database
        # over from nothing every time the app is rebuilt, silently.
        config = _config()
        for entity in config["entities"]:
            if entity["name"] == "store":
                entity["settings"] = {"file": "store/data/app.db"}
        compose = self._compose(config)
        self.assertIn("store-data", compose["volumes"])
        self.assertIn("store-data:/app/store/data", compose["services"]["store"]["volumes"])

    def test_an_entity_on_an_engine_gets_no_second_data_volume(self):
        # Its data belongs to the engine, which has one of its own.
        config = _with_engine()
        for entity in config["entities"]:
            if entity["name"] == "store":
                entity["settings"] = {"file": "store/data/app.db"}
        self.assertEqual(docker.embedded_data_dirs(config), {})

    def test_the_bundle_is_mounted_read_only_when_it_comes_from_the_host(self):
        compose = self._compose(_config(), client="host")
        mounts = compose["services"]["web"]["volumes"]
        self.assertTrue(any(mount.endswith("/build/client:ro") for mount in mounts), mounts)

    def test_the_bundle_is_not_mounted_when_the_image_builds_it(self):
        compose = self._compose(_config(), client="image")
        mounts = compose["services"]["web"]["volumes"]
        self.assertFalse(any("build/client" in mount for mount in mounts), mounts)

    def test_an_engine_shares_its_entitys_namespace_and_holds_the_address(self):
        # The arrangement that keeps a database password off the wire without switching off
        # the guard that says so. The engine holds the address because it has to start
        # first: the entity waits for it to be healthy, and a namespace has to exist before
        # anything joins it.
        config = _with_engine()
        addresses = docker.mesh_addresses(config)
        services = self._compose(config)["services"]
        self.assertEqual(services["store"]["network_mode"], "service:store-postgres")
        self.assertNotIn("networks", services["store"])
        self.assertEqual(
            services["store-postgres"]["networks"]["synqt"]["ipv4_address"],
            addresses["store"])

    def test_an_engine_publishes_no_port(self):
        services = self._compose(_with_engine())["services"]
        self.assertNotIn("ports", services["store-postgres"])

    def test_an_entity_waits_for_its_engine_and_still_for_its_certificate(self):
        # The engine's condition replaces the merged mapping rather than adding to it, so
        # this is the regression guard for dropping the certificate wait by restating
        # depends_on.
        depends = self._compose(_with_engine())["services"]["store"]["depends_on"]
        self.assertEqual(set(depends), {"mesh-init", "store-postgres"})
        self.assertEqual(depends["store-postgres"]["condition"], "service_healthy")

    def test_the_engine_reads_the_same_env_file_the_entity_does(self):
        # One value, written once, serving both ends of the connection. Compose's own
        # ${...} interpolation would read a root .env, which is not where a SynQt secret is.
        compose = self._compose(_with_engine())
        entity_env = compose["services"]["store"]["env_file"]
        engine_env = compose["services"]["store-postgres"]["env_file"]
        self.assertEqual(entity_env, engine_env)
        self.assertEqual(entity_env[0]["path"], "store/.env")

    def test_a_redis_password_is_left_for_the_container_shell_to_expand(self):
        # `$$` in the file is how compose is told to hand a literal `$` to the container,
        # so the shell there expands it from what env_file put in the environment. Written
        # as a single `$`, compose would expand it itself at config time, from the host
        # environment where the value is not (and must not be), and start an open Redis.
        config = _with_engine("redis", "REDIS_PASSWORD")
        written = docker.render_compose(config, docker.mesh_addresses(config))
        self.assertIn('$$REDIS_PASSWORD', written)
        command = yaml.safe_load(written)["services"]["store-redis"]["command"]
        self.assertIn("--requirepass", command[-1])


class DockerignoreTest(unittest.TestCase):
    def test_the_mesh_keys_and_the_env_files_never_enter_the_build_context(self):
        # A file in the build context is a file in an image layer, readable by anyone with
        # the image, whether or not a later stage deletes it.
        ignored = docker.render_dockerignore()
        self.assertIn("synqt/mesh/", ignored)
        self.assertIn("**/.env", ignored)

    def test_a_host_build_tree_never_enters_the_build_context(self):
        self.assertIn("build/", docker.render_dockerignore())


class DockerfileTest(unittest.TestCase):
    def test_the_wasm_kit_is_only_provisioned_when_the_image_builds_the_client(self):
        image = docker.render_dockerfile(_config(), client="image")
        host = docker.render_dockerfile(_config(), client="host")
        self.assertIn("emsdk", image)
        self.assertNotIn("emsdk", host)
        self.assertIn("--client none", host)

    def test_qtremoteobjects_is_built_into_the_wasm_kit(self):
        # The prebuilt WebAssembly kits ship QtWebSockets but not QtRemoteObjects, so
        # without this the client cannot link a single connect point.
        image = docker.render_dockerfile(_config(), client="image")
        self.assertIn("qtremoteobjects", image)
        self.assertIn("QT_HOST_PATH", image)

    def test_the_multithreaded_kit_is_selected_from_the_config(self):
        config = _config(build={"client_threads": "multi"})
        self.assertIn("wasm_multithread", docker.render_dockerfile(config))
        self.assertIn("wasm_singlethread", docker.render_dockerfile(_config()))

    def test_the_runtime_stage_carries_no_compiler(self):
        lines = docker.render_dockerfile(_config()).splitlines()
        runtime = lines[lines.index("FROM debian:bookworm-slim AS runtime"):]
        self.assertNotIn("build-essential", "\n".join(runtime))

    def test_the_image_does_not_run_as_root(self):
        self.assertIn("USER synqt", docker.render_dockerfile(_config()))

    def test_the_mesh_directory_exists_before_the_volume_lands_on_it(self):
        # Docker seeds a fresh named volume from the image's own directory, ownership
        # included. Mounted over a path that does not exist, the volume is created owned by
        # root and the certificate service cannot write into it.
        dockerfile = docker.render_dockerfile(_config())
        self.assertIn("mkdir -p /app/synqt/mesh", dockerfile)
        self.assertLess(dockerfile.index("mkdir -p /app/synqt/mesh"),
                        dockerfile.index("USER synqt"))

    def test_the_runtime_carries_the_library_qt_actually_links(self):
        # libgl1 provides libGL.so.1; Qt links libOpenGL.so.0, which is a different file
        # from a different package. Found by running the thing: every entity died before
        # main() with "error while loading shared libraries", in a restart loop, and the
        # image otherwise looked complete.
        lines = docker.render_dockerfile(_config()).splitlines()
        runtime = "\n".join(lines[lines.index("FROM debian:bookworm-slim AS runtime"):])
        self.assertIn("libopengl0", runtime)

    def test_an_embedded_database_has_a_directory_before_anything_opens_it(self):
        # The sqlite provider opens `<entity>/data/app.db` and does not create the
        # directory, so without this the entity dies at startup on "unable to open database
        # file", which reads like a permissions problem and is not one.
        config = _config()
        for entity in config["entities"]:
            if entity["name"] == "store":
                entity["settings"] = {"file": "store/data/app.db"}
        dockerfile = docker.render_dockerfile(config)
        self.assertIn("/app/store/data", dockerfile)
        self.assertLess(dockerfile.index("/app/store/data"), dockerfile.index("USER synqt"))

    def test_the_cli_is_installed_after_the_project_is_copied(self):
        # A SYNQT_PIP_SPEC naming a path inside the project has to be in the image before
        # pip looks for it.
        dockerfile = docker.render_dockerfile(_config())
        self.assertLess(dockerfile.index("COPY . ."),
                        dockerfile.index("ARG SYNQT_PIP_SPEC"))


class InitTest(unittest.TestCase):
    def _project(self, tmp, config):
        root = Path(tmp)
        (root / "synqt.yaml").write_text(yaml.safe_dump(config))
        return root

    def test_init_writes_every_file_and_refuses_to_clobber(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, _config())
            docker.init(root, _config(), source=None)
            for name in docker.generated_files():
                self.assertTrue((root / name).is_file(), name)
            with self.assertRaises(docker.DockerError) as error:
                docker.init(root, _config(), source=None)
            self.assertIn("--force", str(error.exception))
            docker.init(root, _config(), force=True, source=None)

    def test_init_refuses_a_project_with_no_web_edge(self):
        import tempfile

        config = _config()
        config["entities"] = [e for e in config["entities"] if e["name"] != "web"]
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            with self.assertRaises(docker.DockerError) as error:
                docker.init(root, config, source=None)
            self.assertIn("web edge", str(error.exception))

    def test_init_refuses_a_web_edge_that_owns_an_engine(self):
        # It would have to share a namespace with its engine to keep the link off the wire,
        # and a shared namespace cannot publish the public port.
        import tempfile

        config = _config()
        for entity in config["entities"]:
            if entity["name"] == "web":
                entity["blueprint"] = "persistence"
                entity["provider"] = {"name": "postgres", "password": "env:DB_PASSWORD"}
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            with self.assertRaises(docker.DockerError) as error:
                docker.init(root, config, source=None)
            self.assertIn("network namespace", str(error.exception))

    def test_an_engine_credential_is_generated_rather_than_asked_for(self):
        # Nobody types it and nobody registers it anywhere; it never leaves the pair of
        # containers that share it. A strong random value beats whatever would be typed.
        import tempfile

        config = _with_engine()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            docker.init(root, config, source=None)
            values = docker._read_env(root / "store" / ".env")
        self.assertTrue(len(values["DB_PASSWORD"]) >= 16, values)
        # The engine's image reads it under its own name, out of the same file.
        self.assertEqual(values["POSTGRES_PASSWORD"], values["DB_PASSWORD"])

    def test_a_secret_from_outside_is_left_as_a_placeholder_when_nothing_is_asked(self):
        import tempfile

        config = _config()
        config["identity"] = {"providers": [{"name": "github",
                                             "client_secret": "env:GITHUB_CLIENT_SECRET"}]}
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            docker.init(root, config, source=None)
            values = docker._read_env(root / "web" / ".env")
        self.assertEqual(values["GITHUB_CLIENT_SECRET"], "")

    def test_rerunning_never_resets_a_value_that_was_already_set(self):
        import tempfile

        config = _with_engine()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            docker.init(root, config, source=None)
            first = docker._read_env(root / "store" / ".env")["DB_PASSWORD"]
            docker.init(root, config, force=True, source=None)
            second = docker._read_env(root / "store" / ".env")["DB_PASSWORD"]
        self.assertEqual(first, second)


class SecretDiscoveryTest(unittest.TestCase):
    def test_an_entity_env_reference_is_found(self):
        config = _with_engine()
        self.assertEqual(docker.secret_names(config)["store"], ["DB_PASSWORD"])

    def test_the_identity_secret_is_attributed_to_whoever_runs_identity(self):
        config = _config()
        config["identity"] = {"providers": [{"name": "github",
                                             "client_secret": "env:GITHUB_CLIENT_SECRET"}]}
        self.assertEqual(docker.secret_names(config)["web"], ["GITHUB_CLIENT_SECRET"])

    def test_the_identity_secret_follows_a_provider_entity(self):
        config = _config()
        config["entities"].append({"name": "auth", "kind": "service"})
        config["identity"] = {"provider_entity": "auth",
                              "providers": [{"name": "github",
                                             "client_secret": "env:GITHUB_CLIENT_SECRET"}]}
        found = docker.secret_names(config)
        self.assertEqual(found["auth"], ["GITHUB_CLIENT_SECRET"])
        self.assertNotIn("web", found)


class DriveTest(unittest.TestCase):
    def test_up_refuses_before_the_setup_exists(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(docker.DockerError) as error:
                docker.up_command(tmp)
        self.assertIn("synqt docker init", str(error.exception))

    def test_up_refuses_a_mounted_bundle_that_was_never_built(self):
        # The failure without this is the edge serving 404s for its own client, which reads
        # like a broken app rather than a build that was not run.
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self._generated(tmp, client="host")
            with self.assertRaises(docker.DockerError) as error:
                docker.up_command(root)
        self.assertIn("synqt build --client wasm", str(error.exception))

    def test_up_is_content_when_the_image_builds_the_bundle(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self._generated(tmp, client="image")
            command = docker.up_command(root)
        self.assertEqual(command[-2:], ["up", "--build"])

    def _generated(self, tmp, *, client):
        root = Path(tmp)
        (root / "synqt.yaml").write_text(yaml.safe_dump(_config()))
        docker.init(root, _config(), client=client, source=None)
        return root


if __name__ == "__main__":
    unittest.main()
