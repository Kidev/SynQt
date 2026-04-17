# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolution of ``build.client_cache``: how a repeat visitor gets the client back."""

import unittest

from synqt import check, clientcache, clientshell, maingen


def _config(**build):
    return {"project": {"name": "app"},
            "entities": [{"name": "client", "type": "client", "targets": ["wasm"]},
                         {"name": "web", "type": "web_edge"}],
            "build": dict(build)}


class ModeTest(unittest.TestCase):
    def test_default_is_the_service_worker(self):
        self.assertEqual(clientcache.mode({}), "service_worker")
        self.assertTrue(clientcache.uses_service_worker({}))

    def test_http_mode_drops_the_worker(self):
        config = _config(client_cache="http")
        self.assertEqual(clientcache.mode(config), "http")
        self.assertFalse(clientcache.uses_service_worker(config))

    def test_mode_is_case_insensitive(self):
        self.assertEqual(clientcache.mode(_config(client_cache="HTTP")), "http")


class ValidateTest(unittest.TestCase):
    def test_a_known_mode_is_accepted(self):
        ok, messages = check.validate(_config(client_cache="http"))
        self.assertTrue(ok, messages)

    def test_an_unknown_mode_is_rejected(self):
        ok, messages = check.validate(_config(client_cache="magic"))
        self.assertFalse(ok)
        self.assertTrue(any(m.startswith("error:") and "client_cache" in m
                            for m in messages), messages)


class WorkerTest(unittest.TestCase):
    def _worker(self):
        return clientshell.render_service_worker_js()

    def test_precaches_and_serves_cache_first(self):
        worker = self._worker()
        self.assertIn("caches.open", worker)
        self.assertIn("addAll", worker)
        self.assertIn("caches.match", worker)

    def test_probes_the_manifest_without_caching_the_probe(self):
        worker = self._worker()
        # A cached probe could never observe a new build, which is its whole job.
        self.assertIn("synqt-manifest.json", worker)
        self.assertIn('"no-store"', worker)
        self.assertIn("build_id", worker)

    def test_names_the_cache_after_the_build_and_sweeps_the_rest(self):
        worker = self._worker()
        self.assertIn("caches.delete", worker)
        self.assertIn("skipWaiting", worker)

    def test_carries_the_spdx_header_and_is_csp_clean(self):
        worker = self._worker()
        self.assertIn("SPDX-License-Identifier: Apache-2.0", worker)
        self.assertNotIn("importScripts(", worker)

    def test_a_builds_presence_is_judged_by_content_not_by_cache_name(self):
        # caches.open() creates the cache at install start, so presence is judged by
        # content, not by caches.has(name).
        worker = self._worker()
        self.assertIn("hasCompleteBuild", worker)
        self.assertNotIn("caches.has(", worker)

    def test_failures_are_reported_rather_than_swallowed(self):
        # A cache that silently never updates looks exactly like one that works.
        worker = self._worker()
        self.assertIn("console.warn", worker)

    def test_precache_goes_to_the_network_not_the_browser_http_cache(self):
        # Precache with `cache: "reload"`: a plain addAll() goes through the HTTP cache and
        # would store the previous build under the new name.
        worker = self._worker()
        self.assertIn('cache: "reload"', worker)

    def test_a_new_build_sweeps_the_old_one_without_waiting_for_activate(self):
        # The sweep runs on every build change, not only on activate: the worker script
        # rarely changes, and caches.match() searches caches in creation order.
        worker = self._worker()
        message_handler = worker.split('"synqt-check-update"')[1]
        self.assertIn("sweepOtherCaches", message_handler)


class BootRegistrationTest(unittest.TestCase):
    def _boot(self, **build):
        return clientshell.render_boot_js("client", {"build": dict(build)})

    def test_service_worker_mode_registers_the_worker(self):
        boot = self._boot()
        self.assertIn("serviceWorker.register", boot)
        self.assertIn("synqt-sw.js", boot)

    def test_http_mode_registers_nothing(self):
        boot = self._boot(client_cache="http")
        self.assertNotIn("serviceWorker.register", boot)

    def test_registration_requires_a_secure_context(self):
        # A worker needs https or localhost; the registration is guarded.
        self.assertIn("isSecureContext", self._boot())

    def test_update_defaults_to_reload_when_the_app_does_not_handle_it(self):
        boot = self._boot()
        self.assertIn("__synqtUpdateReady", boot)
        self.assertIn("location.reload", boot)


class EdgeConfigTest(unittest.TestCase):
    """The bundle renderer and the edge CSP agree on whether there is a worker."""

    def _edge_main(self, **build):
        edge = {"name": "web", "type": "web_edge"}
        config = {"project": {"name": "app"},
                  "entities": [{"name": "client", "type": "client", "targets": ["wasm"]},
                               edge],
                  "build": dict(build)}
        return maingen.render_edge_main(config, edge)

    def test_default_edge_expects_the_worker(self):
        self.assertIn("config.serviceWorker = true;", self._edge_main())

    def test_http_mode_tells_the_edge_there_is_no_worker(self):
        self.assertIn("config.serviceWorker = false;",
                      self._edge_main(client_cache="http"))


if __name__ == "__main__":
    unittest.main()


def test_two_bundles_get_different_cache_names():
    app = clientshell.render_service_worker_js("app")
    gate = clientshell.render_service_worker_js("gate")
    assert '"app"' in app
    assert '"gate"' in gate
    # Two bundles get different worker bytes, so a scope change installs the new one.
    assert app != gate


def test_the_default_bundle_name_is_stable():
    # A single-client project's worker is stable across builds.
    assert clientshell.render_service_worker_js() == clientshell.render_service_worker_js()


def test_the_worker_still_sweeps_every_synqt_cache():
    # Per-bundle names, global sweep.
    worker = clientshell.render_service_worker_js("app")
    assert 'PREFIX = "synqt-"' in worker
    assert "caches.delete" in worker


def test_the_warm_script_reads_the_real_manifest_keys():
    script = clientshell.render_warm_script()
    assert "synqtWarmBundle" in script
    # The keys manifest.manifest() writes.
    assert "wasm_size" in script
    assert ".wasm" in script or "manifest.wasm" in script
    assert "files" in script


def test_the_warm_script_reports_progress_and_survives_failure():
    script = clientshell.render_warm_script()
    assert "onProgress" in script
    assert "catch" in script
