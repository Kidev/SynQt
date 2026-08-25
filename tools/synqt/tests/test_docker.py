# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What `synqt docker` generates. The image build itself is not run (it downloads a Qt kit);
the addresses entities dial, the one published port, the build context contents, and the
engine sidecar arrangement are asserted.
"""

import os
import unittest
from pathlib import Path

import yaml

from synqt import docker


def _config(**overrides):
    """A two-entity project: a client, a web edge, and a relational entity."""
    config = {
        "project": {"name": "shop"},
        "entities": [
            {"name": "client", "type": "client", "edge": "web"},
            {"name": "web", "type": "web_edge",
             "public": {"port": 8443}},
            {"name": "store", "type": "relational"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
            {"owner": "store", "consumers": ["web"]},
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
        # Addresses are stable across regeneration.
        self.assertEqual(docker.mesh_addresses(_config()),
                         docker.mesh_addresses(_config()))

    def test_addresses_start_clear_of_the_gateway(self):
        first = docker.mesh_addresses(_config())["web"]
        self.assertTrue(first.endswith(f".{docker.FIRST_HOST}"), first)

    def test_a_subnet_too_small_is_refused_with_the_numbers(self):
        config = _config()
        config["entities"] += [{"name": f"svc{index}", "type": "service"}
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
        # A mesh endpoint is a QHostAddress (src/service/entityruntime.cpp); a service name
        # would parse to a null address.
        import ipaddress

        for entity in self._profile(_config())["entities"]:
            ipaddress.ip_address(entity["mesh"]["host"])

    def test_the_profile_only_changes_what_containers_change(self):
        # A profile only changes and adds (config.merge).
        profile = self._profile(_config())
        self.assertEqual(set(profile), {"entities"})
        for entity in profile["entities"]:
            self.assertTrue(set(entity) - {"name"} <= {"mesh", "public", "tls", "provider"},
                            entity)

    def test_the_edge_gets_a_certificate_it_will_actually_have(self):
        # The scaffolded `tls:` points at a certificate that does not exist yet; the profile
        # supplies one.
        profile = self._profile(_config())
        web = next(e for e in profile["entities"] if e["name"] == "web")
        self.assertEqual(web["tls"]["cert_file"], docker.EDGE_CERT)
        self.assertEqual(web["tls"]["key_file"], docker.EDGE_KEY)

    def test_the_edge_is_told_where_a_browser_reaches_it(self):
        # The edge origin is set explicitly: a container binds 0.0.0.0, which would become
        # the OAuth redirect_uri, `self` in the origin check and the CSP sync endpoint.
        profile = self._profile(_config())
        web = next(e for e in profile["entities"] if e["name"] == "web")
        self.assertEqual(web["public"]["origin"], "https://localhost:8443")

    def test_the_declared_origin_follows_the_port_that_is_published(self):
        # `--port` moves the origin with the published port.
        config = _config()
        profile = yaml.safe_load(
            docker.render_profile(config, docker.mesh_addresses(config), port=9443))
        web = next(e for e in profile["entities"] if e["name"] == "web")
        self.assertEqual(web["public"]["origin"], "https://localhost:9443")

    def test_the_callback_to_register_is_written_next_to_it(self):
        # The redirect_uri must be registered with the provider, character for character.
        config = _config(identity={"providers": [{"name": "github", "client_id": "x"}]})
        rendered = docker.render_profile(config, docker.mesh_addresses(config))
        self.assertIn("https://localhost:8443/auth/callback", rendered)

    def test_a_project_with_no_login_is_not_told_about_a_callback(self):
        rendered = docker.render_profile(_config(), docker.mesh_addresses(_config()))
        self.assertNotIn("/auth/callback", rendered)

    def test_the_browser_certificate_cannot_be_a_mesh_certificate(self):
        # The browser certificate has its own directory: `synqt mesh cert --all` writes
        # `synqt/mesh/<entity>.crt`, which would collide with an edge named `edge`.
        for entity in ("edge", "web", "browser", "localhost"):
            self.assertNotEqual(docker.EDGE_CERT, f"synqt/mesh/{entity}.crt")
            self.assertNotEqual(docker.EDGE_KEY, f"synqt/mesh/{entity}.key")
        self.assertEqual(Path(docker.EDGE_CERT).parent, Path(docker.BROWSER_CERT_DIR))
        self.assertNotEqual(Path(docker.BROWSER_CERT_DIR), Path("synqt/mesh"))

    def test_the_entrypoint_issues_the_browser_certificate_where_it_is_read_from(self):
        # Both halves use the same constants, and the directory exists before writing (`set
        # -eu`).
        script = docker.render_entrypoint("web")
        self.assertIn(f"mkdir -p {docker.BROWSER_CERT_DIR}", script)
        self.assertIn(f"-out {docker.EDGE_CERT}", script)
        self.assertIn(f"-keyout {docker.EDGE_KEY}", script)
        self.assertLess(script.index(f"mkdir -p {docker.BROWSER_CERT_DIR}"),
                        script.index(f"-keyout {docker.EDGE_KEY}"))

    def test_only_the_edge_gets_a_browser_certificate(self):
        profile = self._profile(_config())
        store = next(e for e in profile["entities"] if e["name"] == "store")
        self.assertNotIn("tls", store)

    def test_an_engine_backed_entity_is_pointed_at_loopback(self):
        # Loopback, not the engine service name: the engine shares this namespace, which the
        # release guard accepts (ProviderConfig::isLoopbackHost).
        profile = self._profile(_with_engine())
        store = next(e for e in profile["entities"] if e["name"] == "store")
        self.assertEqual(store["provider"]["host"], "127.0.0.1")
        self.assertEqual(store["provider"]["sslmode"], "disable")


class ComposeTest(unittest.TestCase):
    def _compose(self, config, **kwargs):
        return yaml.safe_load(
            docker.render_compose(config, docker.mesh_addresses(config), **kwargs))

    def test_only_the_web_edge_publishes_a_port(self):
        # Only the edge publishes a port.
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
        # A volume, so a rebuild keeps the database.
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
        # The engine holds the address and starts first; the entity joins its namespace once
        # it is healthy.
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
        # Restating depends_on replaces the merged mapping; the certificate wait must still
        # be there.
        depends = self._compose(_with_engine())["services"]["store"]["depends_on"]
        self.assertEqual(set(depends), {"mesh-init", "store-postgres"})
        self.assertEqual(depends["store-postgres"]["condition"], "service_healthy")

    def test_the_engine_reads_the_same_env_file_the_entity_does(self):
        # One value for both ends, through env_file, not compose ${...} interpolation.
        compose = self._compose(_with_engine())
        entity_env = compose["services"]["store"]["env_file"]
        engine_env = compose["services"]["store-postgres"]["env_file"]
        self.assertEqual(entity_env, engine_env)
        self.assertEqual(entity_env[0]["path"], "db/relational/store/.env")

    def test_a_redis_password_is_left_for_the_container_shell_to_expand(self):
        # `$$` passes a literal `$` to the container shell; a single `$` would be expanded
        # by compose from the host environment.
        config = _with_engine("redis", "REDIS_PASSWORD")
        written = docker.render_compose(config, docker.mesh_addresses(config))
        self.assertIn('$$REDIS_PASSWORD', written)
        command = yaml.safe_load(written)["services"]["store-redis"]["command"]
        self.assertIn("--requirepass", command[-1])


class DockerignoreTest(unittest.TestCase):
    def test_the_mesh_keys_and_the_env_files_never_enter_the_build_context(self):
        # A file in the build context ends up in an image layer.
        ignored = docker.render_dockerignore()
        self.assertIn("synqt/mesh/", ignored)
        self.assertIn("**/.env", ignored)

    def test_a_host_build_tree_never_enters_the_build_context(self):
        self.assertIn("build/", docker.render_dockerignore())


def _run_instructions(dockerfile: str) -> list:
    """The RUN instructions, each rejoined from its backslash continuations, so comments are
    not matched.
    """
    runs = []
    pending = None
    for line in dockerfile.splitlines():
        if pending is not None:
            pending += " " + line.strip().rstrip("\\").strip()
            if not line.rstrip().endswith("\\"):
                runs.append(pending)
                pending = None
            continue
        if line.startswith("RUN "):
            body = line[4:].strip()
            if line.rstrip().endswith("\\"):
                pending = body.rstrip("\\").strip()
            else:
                runs.append(body)
    if pending is not None:
        runs.append(pending)
    return runs


class DockerfileTest(unittest.TestCase):
    def test_the_wasm_kit_is_only_provisioned_when_the_image_builds_the_client(self):
        image = docker.render_dockerfile(_config(), client="image")
        host = docker.render_dockerfile(_config(), client="host")
        self.assertIn("emsdk", image)
        self.assertNotIn("emsdk", host)
        self.assertIn("--client none", host)

    def test_qtremoteobjects_is_built_into_the_wasm_kit(self):
        # The prebuilt WebAssembly kits have no QtRemoteObjects.
        image = docker.render_dockerfile(_config(), client="image")
        self.assertIn("qtremoteobjects", image)
        self.assertIn("QT_HOST_PATH", image)

    def test_the_wasm_kit_gets_its_executable_bit_back(self):
        # Scripts from the WebAssembly archive (qt-cmake among them) arrive 0644; they are
        # made executable.
        image = docker.render_dockerfile(_config(), client="image")
        self.assertIn('chmod +x "$QT_ROOT/$QT_VERSION/wasm_singlethread/bin/"*', image)

    def test_emsdk_is_sourced_from_its_own_directory(self):
        # RUN uses /bin/sh, where emsdk_env.sh cannot find itself through $BASH_SOURCE; it
        # is sourced from its own directory.
        runs = [r for r in _run_instructions(docker.render_dockerfile(_config(), client="image"))
                if "emsdk_env.sh" in r]
        self.assertTrue(runs, "the image never sets up Emscripten")
        for run in runs:
            self.assertIn("cd /opt/emsdk && . ./emsdk_env.sh", run)

    def test_the_client_build_returns_to_the_project_directory(self):
        # Back to the project after the cd into /opt/emsdk.
        runs = [r for r in _run_instructions(docker.render_dockerfile(_config(), client="image"))
                if "synqt build" in r]
        self.assertTrue(runs)
        self.assertIn(f"cd {docker.APP_DIR}", runs[0])

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
        # The directory exists in the image, so a new named volume is seeded with its
        # ownership.
        dockerfile = docker.render_dockerfile(_config())
        self.assertIn("mkdir -p /app/synqt/mesh", dockerfile)
        self.assertLess(dockerfile.index("mkdir -p /app/synqt/mesh"),
                        dockerfile.index("USER synqt"))

    def test_the_runtime_carries_the_library_qt_actually_links(self):
        # libOpenGL.so.0 comes from libopengl0, not libgl1.
        lines = docker.render_dockerfile(_config()).splitlines()
        runtime = "\n".join(lines[lines.index("FROM debian:bookworm-slim AS runtime"):])
        self.assertIn("libopengl0", runtime)

    def test_an_embedded_database_has_a_directory_before_anything_opens_it(self):
        # The sqlite provider does not create `<entity>/data/`.
        config = _config()
        for entity in config["entities"]:
            if entity["name"] == "store":
                entity["settings"] = {"file": "store/data/app.db"}
        dockerfile = docker.render_dockerfile(config)
        self.assertIn("/app/store/data", dockerfile)
        self.assertLess(dockerfile.index("/app/store/data"), dockerfile.index("USER synqt"))

    def test_the_cli_is_installed_after_the_project_is_copied(self):
        # A SYNQT_PIP_SPEC path inside the project is copied before pip runs.
        dockerfile = docker.render_dockerfile(_config())
        self.assertLess(dockerfile.index("COPY . ."),
                        dockerfile.index("ARG SYNQT_PIP_SPEC"))

    def test_the_image_installs_the_checkout_and_not_the_published_name(self):
        # The image installs the CLI from the checkout, not from PyPI.
        dockerfile = docker.render_dockerfile(_config(), from_checkout=True)
        self.assertIn(f"ARG SYNQT_PIP_SPEC={docker.LOCAL_PIP_SPEC}", dockerfile)
        self.assertNotIn("ARG SYNQT_PIP_SPEC=synqt", dockerfile)

    def test_every_directory_the_install_needs_is_copied_before_pip_runs(self):
        # The backend vendors src/, cmake/ and tools/synqtc/, so all must be copied.
        dockerfile = docker.render_dockerfile(_config(), from_checkout=True)
        for name, _, into in docker.SYNQT_CONTEXTS:
            line = f"COPY --from={name} . {into}"
            self.assertIn(line, dockerfile)
            self.assertLess(dockerfile.index(line),
                            dockerfile.index('pip install "$SYNQT_PIP_SPEC"'))

    def test_without_a_checkout_it_reaches_for_the_published_distribution(self):
        dockerfile = docker.render_dockerfile(_config(), from_checkout=False)
        self.assertIn(f"ARG SYNQT_PIP_SPEC={docker.PUBLISHED_PIP_SPEC}", dockerfile)
        self.assertNotIn("COPY --from=synqt-cli", dockerfile)


class CheckoutContextTest(unittest.TestCase):
    """The checkout the image is built from reaches every build that needs it."""

    def test_each_build_is_handed_the_directories_the_dockerfile_copies(self):
        # Every COPY --from context is declared in compose, in both build blocks (mesh-init
        # builds the same image).
        config = _config()
        compose = docker.render_compose(config, docker.mesh_addresses(config),
                                        checkout=Path("/checkout"))
        for name, where, _ in docker.SYNQT_CONTEXTS:
            self.assertEqual(compose.count(f"{name}: ${{SYNQT_SRC:-/checkout}}/{where}"), 2)

    def test_no_context_is_the_whole_checkout(self):
        # Narrow contexts: a context is sent whole before the first COPY.
        self.assertTrue(all(where for _, where, _ in docker.SYNQT_CONTEXTS))

    def test_a_project_with_no_checkout_declares_no_contexts(self):
        config = _config()
        compose = docker.render_compose(config, docker.mesh_addresses(config),
                                        checkout=None)
        self.assertNotIn("additional_contexts", compose)
        self.assertIn(f"SYNQT_PIP_SPEC:-{docker.PUBLISHED_PIP_SPEC}", compose)


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
            for name in docker.generated_files(_config()):
                self.assertTrue((root / name).is_file(), name)
            # No balancer configuration without replicas.
            self.assertFalse((root / f"{docker.DOCKER_DIR}/{docker.FRONT_FILE}").exists())
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
        # A web edge with its own engine is refused: a shared namespace cannot publish the
        # port.
        import tempfile

        # Written by hand: `synqt check` refuses it too (a web_edge has no relational
        # provider).
        config = _config()
        for entity in config["entities"]:
            if entity["name"] == "web":
                entity["provider"] = {"name": "postgres", "password": "env:DB_PASSWORD"}
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            with self.assertRaises(docker.DockerError) as error:
                docker.init(root, config, source=None)
            self.assertIn("network namespace", str(error.exception))

    def test_an_engine_credential_is_generated_rather_than_asked_for(self):
        # Engine credentials are generated.
        import tempfile

        config = _with_engine()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            docker.init(root, config, source=None)
            values = docker._read_env(root / "db" / "relational" / "store" / ".env")
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
            values = docker._read_env(root / "web" / "web" / ".env")
        self.assertEqual(values["GITHUB_CLIENT_SECRET"], "")

    @unittest.skipIf(os.name == "nt", "a POSIX file mode")
    def test_an_entity_env_is_readable_by_its_owner_alone(self):
        # It holds generated engine passwords and the OAuth client secret. A file written
        # with the umask is readable by every account on the host, and one that was already
        # there is tightened when it is rewritten.
        import stat
        import tempfile

        config = _with_engine()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            env = root / "db" / "relational" / "store" / ".env"
            env.parent.mkdir(parents=True, exist_ok=True)
            env.write_text("DB_PASSWORD=kept\n", encoding="utf-8")
            env.chmod(0o644)
            # A second name for the old, world-readable file. The secrets must never be
            # written into it, not even before its mode is tightened.
            old = env.parent / "old-name"
            os.link(env, old)
            docker.init(root, config, source=None)
            self.assertEqual(stat.S_IMODE(env.stat().st_mode), 0o600)
            self.assertEqual(docker._read_env(env)["DB_PASSWORD"], "kept")
            self.assertEqual(old.read_text(encoding="utf-8"), "DB_PASSWORD=kept\n")
            self.assertEqual(list(env.parent.glob(".env.*")), [])

    def test_rerunning_never_resets_a_value_that_was_already_set(self):
        import tempfile

        config = _with_engine()
        with tempfile.TemporaryDirectory() as tmp:
            root = self._project(tmp, config)
            docker.init(root, config, source=None)
            first = docker._read_env(root / "db" / "relational" / "store" / ".env")["DB_PASSWORD"]
            docker.init(root, config, force=True, source=None)
            second = docker._read_env(root / "db" / "relational" / "store" / ".env")["DB_PASSWORD"]
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
        config["entities"].append({"name": "auth", "type": "service"})
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
        # Without a built bundle the edge would serve 404s for its client.
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self._generated(tmp, client="host")
            with self.assertRaises(docker.DockerError) as error:
                docker.up_command(root)
        self.assertIn("synqt build --client wasm", str(error.exception))

    def test_up_is_content_when_the_image_builds_the_bundle(self):
        # This test reaches `up_command`; the compose binary lookup is stubbed, since the
        # macOS runner has no docker.
        import tempfile
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            root = self._generated(tmp, client="image")
            with mock.patch.object(docker, "compose_command",
                                   return_value=["docker", "compose"]):
                command = docker.up_command(root)
        self.assertEqual(command, ["docker", "compose", "up", "--build"])

    def _generated(self, tmp, *, client):
        root = Path(tmp)
        (root / "synqt.yaml").write_text(yaml.safe_dump(_config()))
        docker.init(root, _config(), client=client, source=None)
        return root


if __name__ == "__main__":
    unittest.main()


def _replicated(count=4):
    """The same project with a replicated edge: N interchangeable processes, one front."""
    config = _config()
    for entity in config["entities"]:
        if entity["name"] == "web":
            entity["replicas"] = count
            entity["public"] = {"port": 8443, "origin": "https://shop.example",
                                "trusted_proxies": ["172.30.0.0/16"]}
    config["connect_points"][0]["behind"] = {"anonymous": "store"}
    return config


def _edge_of(config):
    return next(e for e in config["entities"] if e["name"] == "web")


class ReplicatedEdgeTest(unittest.TestCase):
    """N interchangeable edge processes behind a generated nginx balancer."""

    def test_one_replica_is_one_service_named_as_before(self):
        config = _replicated(count=1)
        self.assertEqual(docker.replica_names(_edge_of(config)), ["web"])
        compose = yaml.safe_load(docker.render_compose(config, docker.mesh_addresses(config)))
        self.assertIn("web", compose["services"])
        self.assertNotIn("front", compose["services"])

    def test_no_replicas_key_is_one_service_named_as_before(self):
        config = _config()
        self.assertEqual(docker.replica_names(_edge_of(config)), ["web"])
        compose = yaml.safe_load(docker.render_compose(config, docker.mesh_addresses(config)))
        self.assertIn("web", compose["services"])
        self.assertNotIn("front", compose["services"])

    def test_four_replicas_are_four_services_and_a_front(self):
        config = _replicated(count=4)
        self.assertEqual(docker.replica_names(_edge_of(config)),
                         ["web-1", "web-2", "web-3", "web-4"])
        compose = yaml.safe_load(docker.render_compose(config, docker.mesh_addresses(config)))
        for name in ("web-1", "web-2", "web-3", "web-4", "front"):
            self.assertIn(name, compose["services"])
        self.assertNotIn("web", compose["services"])

    def test_each_replica_gets_its_own_address(self):
        config = _replicated(count=4)
        addresses = docker.mesh_addresses(config)
        assigned = [addresses[name] for name in docker.replica_names(_edge_of(config))]
        self.assertEqual(len(set(assigned)), 4, assigned)
        # And the front needs one too, or compose cannot place it on a subnet it declared.
        self.assertIn("front", addresses)
        self.assertNotIn(addresses["front"], assigned)

    def test_only_the_front_publishes_the_public_port(self):
        config = _replicated(count=4)
        compose = yaml.safe_load(docker.render_compose(config, docker.mesh_addresses(config)))
        published = {name: service.get("ports")
                     for name, service in compose["services"].items()
                     if service.get("ports")}
        self.assertEqual(list(published), ["front"])
        self.assertEqual(published["front"], ["8443:8443"])

    def test_the_front_config_lists_every_replica(self):
        config = _replicated(count=3)
        addresses = docker.mesh_addresses(config)
        front = docker.render_front_config(config, addresses)
        for name in docker.replica_names(_edge_of(config)):
            self.assertIn(f"server {addresses[name]}:8443;", front)
        # Browser links are long-lived: balance on open connections.
        self.assertIn("least_conn;", front)

    def test_the_front_carries_the_upgrade_and_the_client_address(self):
        config = _replicated(count=3)
        front = docker.render_front_config(config, docker.mesh_addresses(config))
        self.assertIn("proxy_set_header Upgrade $http_upgrade;", front)
        self.assertIn('proxy_set_header Connection "upgrade";', front)
        self.assertIn("proxy_set_header X-Forwarded-For", front)
        self.assertIn("proxy_read_timeout", front)

    def test_an_unreplicated_project_generates_no_front_config(self):
        self.assertEqual(docker.render_front_config(_config(), docker.mesh_addresses(_config())),
                         "")


class ReplicatedInitTest(unittest.TestCase):
    def test_init_writes_the_front_config_for_a_replicated_edge(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "app"
            root.mkdir()
            (root / "synqt.yaml").write_text("project:\n  name: shop\n", encoding="utf-8")
            config = _replicated(count=3)
            docker.init(root, config, source=None)
            for name in docker.generated_files(config):
                self.assertTrue((root / name).is_file(), name)
            front = (root / f"{docker.DOCKER_DIR}/{docker.FRONT_FILE}").read_text()
            self.assertIn("least_conn;", front)
            self.assertEqual(front.count("server 172."), 3)
