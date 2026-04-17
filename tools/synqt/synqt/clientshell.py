# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Render the files the browser loads before the client: the page, the boot script, the shell
cache worker and the dev live-reload hook.

Qt's WebAssembly template boots from an inline `onload` handler, which the edge CSP
(`script-src 'self' 'wasm-unsafe-eval'`) blocks. SynQt's shell has no inline handler or
script; boot logic is in the same-origin synqt-boot.js. ``synqt build`` writes the first
three into the bundle; only ``synqt dev`` writes the fourth.
"""

from __future__ import annotations

import html
import json
from typing import Any, Dict

from . import appmodel, clientcache, loadingpage


def render_client_shell(app_js: str, config: Dict[str, Any], project_dir) -> str:
    """The CSP-clean index.html: external scripts only, no inline handlers.

    The logo and the CSS are inlined so the page paints at once. The default policy already
    allows inline style (`style-src 'self' 'unsafe-inline'`, webedgeconfig.h); a hash would
    disable 'unsafe-inline' under CSP Level 2.
    """
    override = loadingpage.html_override(config, project_dir)
    if override is not None:
        return override.read_text(encoding="utf-8")
    return _CLIENT_SHELL.format(
        title=html.escape(loadingpage.title(config)),
        background=loadingpage.background(config),
        favicon=loadingpage.favicon_data_uri(config, project_dir),
        logo=loadingpage.logo_svg(config, project_dir),
        app_js=html.escape(app_js, quote=True),
    )


_CLIENT_SHELL = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, height=device-height, user-scalable=0"/>
  <title>{title}</title>
  <link rel="icon" type="image/svg+xml" href="{favicon}">
  <style>
    /* One length sizes the column: the mark and the progress track under it. It is
       bounded on both axes, so a short landscape window still shows the logo, the bar
       and the word "Loading". The two clamps set a floor for a very small window and a
       ceiling for a desktop. The gaps and padding scale with the viewport height too. */
    :root {{
      --synqt-mark: clamp(4rem, min(55vw, 40vh), 22rem);
      --synqt-gap: clamp(0.75rem, 3vh, 1.5rem);
    }}
    /* The background is on the document as well as the overlay, because the overlay
       hides a frame or two before the first QML paint.

       `height: 100%` first and `100dvh` second: a mobile browser measures `100%`
       against its current layout viewport. An engine without `dvh` keeps the
       percentage.

       `fixed` and `no-repeat` size the background to the viewport. A root background
       image is placed against the root box and tiled, so it would restart or stop
       whenever the canvas is taller than that box. */
    html, body {{
      padding: 0; margin: 0; overflow: hidden; height: 100%;
      background: {background};
      background-repeat: no-repeat;
      background-attachment: fixed;
    }}
    html, body {{ height: 100dvh }}
    #screen {{ width: 100%; height: 100% }}
    /* Fixed to all four edges, so the background covers the viewport. `min-height`
       handles a retracting URL bar, as above. */
    #synqt-loading {{
      position: fixed; inset: 0; min-height: 100dvh;
      display: flex; flex-direction: column;
      align-items: center; justify-content: center; gap: var(--synqt-gap);
      padding: clamp(1rem, 5vh, 3rem); box-sizing: border-box;
      background: {background}; background-repeat: no-repeat;
      background-attachment: fixed; color: #e8e6f0;
      font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    }}
    #synqt-loading[hidden] {{ display: none }}
    /* `max-height` as well as `width`, because a project logo (`build.loading.logo`)
       need not be square. The SVG keeps its aspect ratio inside the square. */
    #synqt-logo svg {{
      width: var(--synqt-mark); max-width: 100%; max-height: var(--synqt-mark);
      height: auto; display: block;
    }}
    #synqt-track {{
      width: var(--synqt-mark); max-width: 100%;
      height: clamp(3px, 0.5vh, 6px); border-radius: 3px;
      background: rgba(255, 255, 255, 0.16); overflow: hidden;
    }}
    #synqt-bar {{
      width: 0; height: 100%; border-radius: 3px; background: #ffffff;
      transition: width 0.2s ease;
    }}
    #synqt-status {{
      font-size: clamp(0.75rem, 1.8vh, 1rem); opacity: 0.75; letter-spacing: 0.02em;
      text-align: center; max-width: 100%;
    }}
  </style>
</head>
<body>
  <div id="synqt-loading">
    <div id="synqt-logo">{logo}</div>
    <div id="synqt-track"><div id="synqt-bar"></div></div>
    <div id="synqt-status">Loading</div>
    <noscript>JavaScript is disabled. Please enable JavaScript to use this application.</noscript>
  </div>
  <div id="screen"></div>
  <script src="{app_js}"></script>
  <script src="qtloader.js"></script>
  <script src="synqt-boot.js"></script>
</body>
</html>
"""


def render_boot_js(target: str, config: Dict[str, Any]) -> str:
    """The external boot script that compiles and starts the WebAssembly module.

    Eval-free, for the edge CSP. It passes a compileStreaming promise through qtloader's
    ``qt.module`` option, so the page shows real progress while compilation overlaps the
    download. Under ``build.client_cache: service_worker`` it also registers the shell cache
    and forwards its update signal.
    """
    registration = _BOOT_SW_JS if clientcache.uses_service_worker(config) else ""
    return (_BOOT_JS.replace("ENTRY_FUNCTION", "%s_entry" % target)
            .replace("CLIENT_ROUTE", appmodel.client_route(config))
            .replace("// EDGE_ORIGIN", _edge_origin_js(config))
            .replace("// SERVICE_WORKER_HOOK", registration))


def _edge_origin_js(config: Dict[str, Any]) -> str:
    """Publish the edge origin to the page when a CDN serves it (`split_origin`), since
    `window.location` then names the CDN. Emitted only when the project declares it.
    """
    origin = appmodel.public_origin(config)
    if not origin or appmodel.serves_client(config):
        return ""
    return ('// This bundle is delivered from another origin, so the edge names itself\n'
            '    // here rather than being read off the page (public.origin).\n'
            '    window.__synqtEdgeOrigin = %s;' % json.dumps(origin))


_BOOT_SW_JS = """navigator.serviceWorker.register("synqt-sw.js").then(function () {
                return navigator.serviceWorker.ready;
            }).then(function (registration) {
                navigator.serviceWorker.addEventListener("message", function (event) {
                    if (!event.data || event.data.type !== "synqt-update-ready") {
                        return;
                    }
                    // If the app handles App.updateReady, the client runtime installs
                    // this hook and the app decides. Otherwise reload now.
                    if (typeof window.__synqtUpdateReady === "function") {
                        window.__synqtUpdateReady();
                    } else {
                        window.location.reload();
                    }
                });
                if (registration.active) {
                    registration.active.postMessage({ type: "synqt-check-update" });
                }
            }).catch(function (error) {
                // The cache is an optimization; a worker that fails to install never
                // stops the boot.
                console.warn("synqt: service worker unavailable", error);
            });"""


_BOOT_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Boots the Qt WebAssembly module. External (script-src 'self') and eval-free, for the
// edge CSP. Generated by `synqt build`.
(function () {
    "use strict";

    // EDGE_ORIGIN

    var loading = null;
    var bar = null;
    var status = null;
    var screen = null;

    function setProgress(loaded, total) {
        if (!bar || !(total > 0)) {
            return;
        }
        var percent = Math.max(0, Math.min(100, (loaded / total) * 100));
        bar.style.width = percent.toFixed(1) + "%";
        if (status) {
            status.textContent = "Loading " + percent.toFixed(0) + "%";
        }
    }

    // Count the module bytes as they stream to compileStreaming, so compilation still
    // overlaps the download. `total` comes from the manifest: the edge serves the wasm
    // compressed, so the response length is not the decoded size.
    function countingResponse(response, total) {
        if (!response.body || typeof TransformStream === "undefined") {
            return response;
        }
        var loaded = 0;
        var counter = new TransformStream({
            transform: function (chunk, controller) {
                loaded += chunk.byteLength;
                setProgress(loaded, total);
                controller.enqueue(chunk);
            }
        });
        return new Response(response.body.pipeThrough(counter), {
            headers: { "Content-Type": "application/wasm" }
        });
    }

    function compileModule(manifest) {
        return fetch(manifest.wasm, { credentials: "same-origin" }).then(function (response) {
            if (!response.ok) {
                throw new Error("could not fetch " + manifest.wasm + ": " + response.status);
            }
            return WebAssembly.compileStreaming(countingResponse(response, manifest.wasm_size));
        });
    }

    function fail(error) {
        if (status) {
            status.textContent = "Failed to load";
        }
        if (loading) {
            loading.hidden = false;
        }
        console.error(error);
    }

    function start(manifest) {
        return qtLoad({
            qt: {
                // Documented qtloader option: a Promise<WebAssembly.Module>, passed
                // unresolved so the download starts now.
                module: compileModule(manifest),
                onLoaded: function () {
                    if (loading) {
                        loading.hidden = true;
                    }
                },
                onExit: function (exitData) {
                    var suffix = exitData.code !== undefined ? " with code " + exitData.code : "";
                    if (status) {
                        status.textContent = "Application exit" + suffix;
                    }
                    if (loading) {
                        loading.hidden = false;
                    }
                },
                entryFunction: window.ENTRY_FUNCTION,
                containerElements: [screen]
            }
        });
    }

    // Ask the edge for a session before the app connects. Split-origin builds only: the
    // page came from a CDN, so the browser has no edge cookie and the wss upgrade would
    // be refused. The edge answers 204 with a Set-Cookie; `credentials: "include"`
    // stores it, and the edge echoes this exact origin.
    //
    // Never fatal. If a third-party cookie is blocked, the app reports that it cannot
    // connect.
    function bootstrapSession() {
        if (!window.__synqtEdgeOrigin) {
            return Promise.resolve();
        }
        var origin = String(window.__synqtEdgeOrigin)
            .replace(/^wss:/, "https:")
            .replace(/^ws:/, "http:");
        return fetch(origin + "CLIENT_ROUTE", {
            credentials: "include",
            cache: "no-store"
        }).catch(function (error) {
            console.warn("synqt: could not obtain a session from the edge", error);
        });
    }

    function init() {
        loading = document.querySelector("#synqt-loading");
        bar = document.querySelector("#synqt-bar");
        status = document.querySelector("#synqt-status");
        screen = document.querySelector("#screen");

        // The shell cache, when this build has one. A worker needs a secure context
        // (https, or localhost in dev). Registration is off the critical path; the
        // module fetch starts regardless.
        if ("serviceWorker" in navigator && window.isSecureContext) {
            // SERVICE_WORKER_HOOK
        }

        // The session request and the module download overlap. Qt starts only after
        // both, so it never races the cookie.
        Promise.all([
            bootstrapSession(),
            fetch("synqt-manifest.json", { credentials: "same-origin" })
                .then(function (response) { return response.json(); })
        ]).then(function (results) { return start(results[1]); }).catch(fail);
    }

    window.addEventListener("load", init);
})();
"""


def render_service_worker_js(bundle: str = "client") -> str:
    """The service worker that makes a repeat visit instant, for one bundle.

    Cache-first over CacheStorage, with the manifest build_id as the cache name; old caches
    are swept on activate. The update check is one no-store fetch of the manifest.

    The bundle name is written into the script: an edge may serve two bundles at "/" (before
    and after sign-in), and the browser reinstalls a worker only when its bytes change. The
    sweep removes every other `synqt-` cache.
    """
    return _SERVICE_WORKER_JS.replace("__SYNQT_BUNDLE__", bundle)


_SERVICE_WORKER_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The SynQt client shell cache. Generated by `synqt build`. Do not edit. Cache-first,
// so a repeat visit needs no network before the app starts; a background manifest check
// looks for a new build.
"use strict";

var MANIFEST = "synqt-manifest.json";
var PREFIX = "synqt-";
// Which bundle this worker belongs to (see render_service_worker_js). Two bundles at
// the same scope get distinct workers.
var BUNDLE = "__SYNQT_BUNDLE__";

function cacheName(buildId) {
    return PREFIX + BUNDLE + "-" + buildId;
}

// The manifest identifies a build, so it is fetched no-store.
function fetchManifest() {
    return fetch(MANIFEST, { cache: "no-store", credentials: "same-origin" })
        .then(function (response) {
            if (!response.ok) {
                throw new Error("manifest fetch failed: " + response.status);
            }
            return response.json();
        });
}

function precache(manifest) {
    return caches.open(cacheName(manifest.build_id)).then(function (cache) {
        var urls = manifest.files.slice();
        if (urls.indexOf(MANIFEST) === -1) {
            urls.push(MANIFEST);
        }
        // cache: "reload" is required. A plain addAll() goes through the HTTP cache and
        // could store the previous build's bytes under this build's name.
        var requests = urls.map(function (url) {
            return new Request(url, { cache: "reload", credentials: "same-origin" });
        });
        return cache.addAll(requests);
    });
}

// Whether this build is cached and complete. caches.open() creates the cache when
// install starts, so the name alone proves nothing. addAll() is atomic, so a cached
// manifest means the precache finished.
function hasCompleteBuild(buildId) {
    var name = cacheName(buildId);
    return caches.keys().then(function (names) {
        if (names.indexOf(name) === -1) {
            return false;
        }
        return caches.open(name).then(function (cache) {
            return cache.match(MANIFEST);
        }).then(function (hit) {
            return Boolean(hit);
        });
    });
}

function sweepOtherCaches(keep) {
    return caches.keys().then(function (names) {
        return Promise.all(names.map(function (name) {
            if (name.indexOf(PREFIX) === 0 && name !== keep) {
                return caches.delete(name);
            }
            return null;
        }));
    });
}

self.addEventListener("install", function (event) {
    // Take over as soon as the new build is cached; the page is about to reload onto
    // it.
    event.waitUntil(fetchManifest().then(precache).then(function () {
        return self.skipWaiting();
    }).catch(function (error) {
        // A failed install must not block the worker: the page still boots from the
        // network. Warn, so a bundle that never caches is visible.
        console.warn("synqt: shell precache failed", error);
    }));
});

self.addEventListener("activate", function (event) {
    event.waitUntil(fetchManifest().then(function (manifest) {
        return sweepOtherCaches(cacheName(manifest.build_id));
    }).then(function () {
        return self.clients.claim();
    }).catch(function () {}));
});

self.addEventListener("fetch", function (event) {
    if (event.request.method !== "GET") {
        return;
    }
    // Never serve the probe from cache, and never intercept another origin.
    if (event.request.url.indexOf(MANIFEST) !== -1
        || new URL(event.request.url).origin !== self.location.origin) {
        return;
    }
    event.respondWith(caches.match(event.request).then(function (hit) {
        return hit || fetch(event.request);
    }));
});

self.addEventListener("message", function (event) {
    if (!event.data || event.data.type !== "synqt-check-update") {
        return;
    }
    event.waitUntil(fetchManifest().then(function (manifest) {
        return hasCompleteBuild(manifest.build_id).then(function (current) {
            if (current) {
                return null;  // the common case: nothing changed, stop here
            }
            return precache(manifest).then(function () {
                // Sweep here, not only in activate: the worker script rarely changes,
                // so activate rarely fires, and caches.match() searches caches in
                // creation order.
                return sweepOtherCaches(cacheName(manifest.build_id));
            }).then(function () {
                return self.clients.matchAll();
            }).then(function (clients) {
                clients.forEach(function (client) {
                    client.postMessage({ type: "synqt-update-ready",
                                         buildId: manifest.build_id });
                });
            });
        });
    }).catch(function (error) {
        // A failed probe leaves the cache as it was. Warn, so a cache that never
        // updates is visible.
        console.warn("synqt: update check failed", error);
    }));
});
"""


# the gate's bundle warm-up

_WARM_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Fetch the bundle the caller is now entitled to, while they are still on the sign-in
// page. Generated by `synqt build`. Do not edit.
"use strict";

window.synqtWarmBundle = function (onProgress) {
    var report = typeof onProgress === "function" ? onProgress : function () {};
    return fetch("synqt-manifest.json", { cache: "no-store", credentials: "same-origin" })
        .then(function (response) {
            if (!response.ok) {
                throw new Error("manifest fetch failed: " + response.status);
            }
            return response.json();
        })
        .then(function (manifest) {
            var total = manifest.wasm_size || 0;
            // Progress is measured against the module, the bulk of the transfer.
            // Streamed, so the bar moves.
            return fetch(manifest.wasm, { credentials: "same-origin" })
                .then(function (response) {
                    if (!response.ok || !response.body || !total) {
                        return null;
                    }
                    var reader = response.body.getReader();
                    var done = 0;
                    return (function pump() {
                        return reader.read().then(function (chunk) {
                            if (chunk.done) {
                                return null;
                            }
                            done += chunk.value.length;
                            report(done, total);
                            return pump();
                        });
                    }());
                })
                .then(function () {
                    var rest = (manifest.files || []).filter(function (name) {
                        return name !== manifest.wasm;
                    });
                    return Promise.all(rest.map(function (name) {
                        return fetch(name, { credentials: "same-origin" });
                    }));
                })
                .then(function () {
                    report(total, total);
                    return true;
                });
        })
        .catch(function (error) {
            // A failed warm-up only loses the head start: the navigation fetches the
            // same bundle. Reported as a negative so the caller can drop its progress
            // UI.
            report(-1, 0);
            console.warn("synqt: bundle warm-up failed", error);
            return false;
        });
};
"""


def render_warm_script() -> str:
    """The bundle warm-up a gate runs once a credential is accepted.

    `window.synqtWarmBundle(onProgress)` reads the target manifest, streams the module with
    progress against `wasm_size`, then fetches the other files into the HTTP cache. The
    service worker precache uses `cache: "reload"`, so this does not fill it.
    """
    return _WARM_JS


# the dev live-reload hook

def render_dev_reload_js() -> str:
    """The dev-only live-reload script ``synqt dev`` injects into the served bundle.

    It polls the reload token the watcher bumps after each rebuild and reloads the page when
    it changes. Eval-free and same-origin (``connect-src 'self'``). Never written by ``synqt
    build``.
    """
    return """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Injected by `synqt dev` only. Polls the reload token the file watcher bumps after
// each rebuild and reloads the page when it changes. External and eval-free, for the
// edge CSP; the fetch is same-origin (connect-src 'self').
(function () {
    // Dev has no shell cache (build.client_cache is http), but a production build
    // loaded from this origin may have left its worker installed, which would serve a
    // cached shell over the dev build. Evict it.
    if ("serviceWorker" in navigator) {
        navigator.serviceWorker.getRegistrations().then(function (registrations) {
            registrations.forEach(function (registration) { registration.unregister(); });
        }).catch(function () {});
    }

    "use strict";

    var baseline = null;

    function poll() {
        fetch("synqt-reload.txt", { cache: "no-store" })
            .then(function (response) { return response.text(); })
            .then(function (text) {
                var token = text.trim();
                if (baseline === null) {
                    baseline = token;
                } else if (token !== baseline) {
                    window.location.reload();
                }
            })
            .catch(function () { /* the edge is restarting, keep polling */ });
    }

    window.setInterval(poll, 1000);
    poll();
})();
"""
