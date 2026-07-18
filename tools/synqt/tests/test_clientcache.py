# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolution of ``build.client_cache``: how a repeat visitor gets the client back."""

import unittest

from synqt import check, clientcache


def _config(**build):
    return {"project": {"name": "app"},
            "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]},
                         {"name": "web", "kind": "service", "capability": "web_edge"}],
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
        from synqt import appgen
        return appgen.render_service_worker_js()

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
        # caches.open() creates the named cache the moment install starts, so a failed or
        # in-flight precache leaves an empty cache under the right name. Judging presence
        # by caches.has(name) would call that build "current" and never update again.
        worker = self._worker()
        self.assertIn("hasCompleteBuild", worker)
        self.assertNotIn("caches.has(", worker)

    def test_failures_are_reported_rather_than_swallowed(self):
        # A cache that silently never updates looks exactly like one that works.
        worker = self._worker()
        self.assertIn("console.warn", worker)

    def test_precache_goes_to_the_network_not_the_browser_http_cache(self):
        # A plain addAll() fetches through the HTTP cache, which hands back the previous
        # build's bytes and stores them under the new build's name: a cache labelled new
        # and holding old, so the update silently never takes effect. Verified against a
        # real browser; no string assertion would have caught it.
        worker = self._worker()
        self.assertIn('cache: "reload"', worker)

    def test_a_new_build_sweeps_the_old_one_without_waiting_for_activate(self):
        # The worker script is identical from build to build, so activate fires once ever
        # while build_id changes on every deploy. Sweeping only there would strand a full
        # uncompressed module per deploy, and caches.match() searches every cache in
        # creation order, so the stale one would keep winning and the update would never
        # take effect.
        worker = self._worker()
        message_handler = worker.split('"synqt-check-update"')[1]
        self.assertIn("sweepOtherCaches", message_handler)


class BootRegistrationTest(unittest.TestCase):
    def _boot(self, **build):
        from synqt import appgen
        return appgen.render_boot_js("client", {"build": dict(build)})

    def test_service_worker_mode_registers_the_worker(self):
        boot = self._boot()
        self.assertIn("serviceWorker.register", boot)
        self.assertIn("synqt-sw.js", boot)

    def test_http_mode_registers_nothing(self):
        boot = self._boot(client_cache="http")
        self.assertNotIn("serviceWorker.register", boot)

    def test_registration_requires_a_secure_context(self):
        # A worker needs https or localhost; without the guard a plaintext dev edge
        # throws on every boot.
        self.assertIn("isSecureContext", self._boot())

    def test_update_defaults_to_reload_when_the_app_does_not_handle_it(self):
        boot = self._boot()
        self.assertIn("__synqtUpdateReady", boot)
        self.assertIn("location.reload", boot)


class EdgeConfigTest(unittest.TestCase):
    """One knob drives the whole chain: the bundle renderer and the edge must agree, or
    the CSP would advertise a worker the build never emitted (or block one it did)."""

    def _edge_main(self, **build):
        from synqt import appgen
        edge = {"name": "web", "kind": "service", "capability": "web_edge"}
        config = {"project": {"name": "app"},
                  "entities": [{"name": "client", "kind": "client", "targets": ["wasm"]},
                               edge],
                  "build": dict(build)}
        return appgen.render_edge_main(config, edge)

    def test_default_edge_expects_the_worker(self):
        self.assertIn("config.serviceWorker = true;", self._edge_main())

    def test_http_mode_tells_the_edge_there_is_no_worker(self):
        self.assertIn("config.serviceWorker = false;",
                      self._edge_main(client_cache="http"))


if __name__ == "__main__":
    unittest.main()
