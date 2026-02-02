# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Render the files the browser loads before the client does. The page, the boot
script, the shell cache worker, and the dev live-reload hook.

Qt's default WebAssembly template boots from `<body onload="init()">` with an inline
script. An inline event handler cannot be allowed by a CSP hash, so it violates the
edge's strict `script-src 'self' 'wasm-unsafe-eval'`. SynQt ships its own shell instead:
no inline handler and no inline script. All boot logic lives in an external
synqt-boot.js (served same-origin, so plain `script-src 'self'` admits it). Everything
here holds to that rule, so a page these functions produce loads under the edge's
default policy with nothing relaxed for it.

``synqt build`` writes the first three into the client bundle. The fourth is written
only by ``synqt dev``.
"""

from __future__ import annotations

import html
import json
from typing import Any, Dict

from . import appmodel, clientcache, loadingpage


def render_client_shell(app_js: str, config: Dict[str, Any], project_dir) -> str:
    """The CSP-clean index.html. External scripts only, no inline handlers.

    The logo and the CSS are inlined rather than linked. This page's only job is to
    appear instantly, and a linked asset costs a round trip before it can paint. The
    inline <style> needs no CSP work. The default policy already carries
    style-src 'self' 'unsafe-inline' (webedgeconfig.h). Adding a hash instead would
    silently disable that 'unsafe-inline' for every app, per CSP Level 2.
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
    /* One length drives the whole column: the mark, the progress track under it, and
       through them the height of the three stacked together. It is bounded on both
       axes. Sized from the width alone, a short landscape
       viewport gets a mark taller than the window. At 640x300 the logo is clipped top
       and bottom and the bar and the word "Loading" were off the bottom of the screen
       entirely, on a page whose only job is to tell a visitor something is happening.
       40vh is the cap that cannot happen under, 55vw keeps it from dominating a narrow
       phone, and the two clamps are the floor for a very small window and the ceiling
       that stops a desktop from rendering a 900px logo.

       The rest of the column is proportional to the viewport height for the same reason,
       so the gaps and the padding shrink with the space rather than eating a short
       window. */
    :root {{
      --synqt-mark: clamp(4rem, min(55vw, 40vh), 22rem);
      --synqt-gap: clamp(0.75rem, 3vh, 1.5rem);
    }}
    /* The background belongs on the document, not only on the overlay: the overlay is
       hidden the moment Qt reports the module loaded, which is a frame or two before the
       first QML paint, and the browser's default white would flash through that gap.

       `height: 100%` first and `100dvh` second: a mobile browser's `100%` is measured
       against whichever viewport it currently calls layout, so the page can come up a
       URL-bar short of the screen. The dynamic unit is the one that tracks the bar as it
       retracts, and an engine that does not know it keeps the percentage.

       `fixed` and `no-repeat` are what make the background the viewport's rather than the
       element's. The root element's background is painted over the whole canvas, but an
       image in it is still sized and placed against the root element's own box and then
       tiled to fill the rest; so any moment the canvas is taller than that box -- a
       retracting URL bar, an engine that does not know `dvh`, a window resized before
       layout catches up -- the gradient either restarts partway down or leaves what it
       does not reach. Attached to the viewport it is measured against the thing it has
       to cover. */
    html, body {{
      padding: 0; margin: 0; overflow: hidden; height: 100%;
      background: {background};
      background-repeat: no-repeat;
      background-attachment: fixed;
    }}
    html, body {{ height: 100dvh }}
    #screen {{ width: 100%; height: 100% }}
    /* Fixed and pinned to all four edges, so the background is the viewport's whatever
       the document under it turns out to be. `min-height` is the same dynamic-viewport
       belt as above, for the moment a retracting URL bar makes the window taller than
       the box that was laid out. */
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
    /* `max-height` as well as `width`, because the logo is replaceable
       (`build.loading.logo`) and a project's own mark is not necessarily square. An SVG
       fits itself to its box preserving its aspect ratio, so a tall one letterboxes
       inside the square rather than stretching the column past the window. */
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

    External and eval-free so the edge's strict Content-Security-Policy holds. It hands
    Qt a compileStreaming promise through qtloader's documented ``qt.module`` option,
    which is what lets the page show a real percentage while compilation still overlaps
    the download.

    Under ``build.client_cache: service_worker`` it also registers the shell cache and
    forwards its update signal. Under ``http`` the registration is absent.
    """
    registration = _BOOT_SW_JS if clientcache.uses_service_worker(config) else ""
    return (_BOOT_JS.replace("ENTRY_FUNCTION", "%s_entry" % target)
            .replace("CLIENT_ROUTE", appmodel.client_route(config))
            .replace("// EDGE_ORIGIN", _edge_origin_js(config))
            .replace("// SERVICE_WORKER_HOOK", registration))


def _edge_origin_js(config: Dict[str, Any]) -> str:
    """Publish the edge's origin to the page, when the page is not served from it.

    Under `origin_model: split_origin` a CDN delivers this bundle, so neither the boot
    script nor the client can read the edge from `window.location`: that names the CDN,
    which hosts no sync endpoint and no session. The one place that knows is the build, so
    the build states it here, in the file it already generates and the CDN already serves.

    Emitted only when the project declares it, so a same-origin app generates the boot
    script it generated before this existed and keeps deriving its edge from its own page.
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
                    // The app decides if it asked to (the client runtime installs this
                    // hook when something handles App.updateReady). If nothing did,
                    // reload now. An update nobody applies is worse than an interruption.
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
                // The cache is an optimization. A worker that will not install must never
                // stop the app from booting.
                console.warn("synqt: service worker unavailable", error);
            });"""


_BOOT_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Boots the Qt WebAssembly module. External (script-src 'self') and eval-free so the
// edge's strict Content-Security-Policy holds. Generated by `synqt build`.
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

    // Count the module's bytes as they stream past, without buffering it: the chunks go
    // straight through to compileStreaming, so compilation still overlaps the download.
    // `total` comes from the manifest, never from the response headers: the edge serves
    // the wasm compressed, so its declared length is the compressed size while these
    // chunks are decoded, and the ratio would run past 100%.
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
                // Documented qtloader option: Promise<WebAssembly.Module>. Passing the
                // promise unresolved lets the download start now and the loader await it.
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

    // Ask the edge for a session before the app tries to connect.
    //
    // Only for a split-origin build. This page came from a CDN, so the browser has never
    // touched the edge and holds no cookie for it, and the wss upgrade refuses a request
    // that carries no session. One credentialed request to the edge's client route fixes
    // that. The edge answers 204 and a Set-Cookie. `credentials: "include"` is what both
    // sends nothing yet and stores what comes back, and it is why the edge echoes this
    // exact origin rather than a wildcard.
    //
    // Never fatal. A blocked third-party cookie leaves the app to report a connection it
    // cannot authorize, which is a far clearer failure than a boot that never happens.
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
        // (https, or localhost in dev). Without the guard a plaintext edge throws here
        // on every boot. Registration is off the critical path. The module fetch below
        // starts regardless, and the worker serves it from cache once it controls.
        if ("serviceWorker" in navigator && window.isSecureContext) {
            // SERVICE_WORKER_HOOK
        }

        // The session request and the module download overlap. The module is megabytes
        // and the session is one small round trip, so waiting for both costs nothing over
        // waiting for the module, and starting Qt before the cookie exists would race it.
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

    Cache-first over CacheStorage, with the manifest's build_id as the cache name, so a
    new build lands in its own cache and the old one is swept on activate. The update
    probe is a single no-store fetch of the manifest: identical is the common case and
    costs one small request; only a real difference pulls the module again.

    The bundle name is written into the script, and that is load bearing rather than
    cosmetic. An edge may serve one bundle to an anonymous visitor and another once they
    sign in, both at "/", so both register a worker at the same scope. The browser re-runs
    install only when the worker's own bytes changed; two byte-identical scripts would
    leave the first bundle's worker active, and its cache-first fetch handler would go on
    answering "/" from the bundle the visitor is no longer entitled to. Naming the bundle
    here makes the switch a real update.

    The sweep stays global (every `synqt-` cache but the live one), so changing bundles
    reclaims the other one rather than stranding a full uncompressed module on disk.
    """
    return _SERVICE_WORKER_JS.replace("__SYNQT_BUNDLE__", bundle)


_SERVICE_WORKER_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The SynQt client shell cache. Generated by `synqt build`. Never edit in place.
// Cache-first so a repeat visit reaches the app with no network on the critical path,
// then a background manifest probe decides whether anything changed.
"use strict";

var MANIFEST = "synqt-manifest.json";
var PREFIX = "synqt-";
// Which bundle this worker belongs to. See render_service_worker_js. This is what makes
// two bundles served at the same scope two distinct workers rather than one.
var BUNDLE = "__SYNQT_BUNDLE__";

function cacheName(buildId) {
    return PREFIX + BUNDLE + "-" + buildId;
}

// The manifest is the identity of a build. It is fetched no-store, because a cached
// probe could never observe a new build, which is the one thing it exists to do.
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
        // cache: "reload" is load bearing. A plain addAll() fetches through the browser's
        // HTTP cache, which will happily hand back the *previous* build's bytes and store
        // them under this build's name: a cache labelled new and holding old, so the
        // update silently never takes effect. Going to the network is the only way to be
        // sure the bytes match the build_id they are filed under.
        var requests = urls.map(function (url) {
            return new Request(url, { cache: "reload", credentials: "same-origin" });
        });
        return cache.addAll(requests);
    });
}

// Whether this build is cached *and complete*. The name existing proves nothing:
// caches.open() creates the named cache the moment install starts, so a failed or
// in-flight precache leaves an empty cache under the right name. addAll() is atomic, so
// the manifest being present is what proves the precache finished.
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
    // Take over as soon as the new build is cached. The page that triggered the update
    // is about to reload onto it.
    event.waitUntil(fetchManifest().then(precache).then(function () {
        return self.skipWaiting();
    }).catch(function (error) {
        // A failed install must not wedge the worker. The page still boots from the
        // network, because the cache is an optimization and never a dependency. Warn
        // rather than swallow, or a bundle that never caches looks exactly like one that
        // does.
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
                // Sweep here, not only in activate. The worker script is identical from
                // build to build, so activate fires once ever while build_id changes on
                // every deploy. Without this, each deploy would strand another cache
                // holding a full uncompressed module, and caches.match() searches every
                // cache in creation order, so the stale one would keep winning and the
                // update would never take effect.
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
        // A failed probe leaves the working cache exactly as it was. Warn rather than
        // swallow. A cache that silently never updates is the worst outcome here.
        console.warn("synqt: update check failed", error);
    }));
});
"""


# the gate's bundle warm-up

_WARM_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Fetch the bundle the caller has become entitled to, while they are still looking
// at the page that signed them in. Generated by `synqt build`. Never edit in place.
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
            // The module is the transfer. Everything else together is noise beside it, so
            // it is the one thing progress is measured against. Streamed rather than
            // awaited whole, or the bar would sit at zero and then jump to done.
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
            // A warm-up that fails is not the visitor's problem: the navigation that
            // follows fetches the same bundle, only without the head start. Reported as a
            // negative so a caller can drop its progress UI rather than leave it stuck.
            report(-1, 0);
            console.warn("synqt: bundle warm-up failed", error);
            return false;
        });
};
"""


def render_warm_script() -> str:
    """The bundle warm-up a gate runs once a credential has been accepted.

    Exposed as `window.synqtWarmBundle(onProgress)`. It reads the target bundle's
    manifest, streams the module reporting progress against `wasm_size`, then fetches the
    rest of the files. The navigation that follows finds them in the browser's HTTP cache,
    so the visitor waits at a page with a progress bar instead of at a blank one.

    It does not fill the service worker's cache. That
    precache fetches with `cache: "reload"` (see render_service_worker_js), so
    it always goes to the network and cannot be primed from here. This shortens first
    paint, not the precache.
    """
    return _WARM_JS


# the dev live-reload hook

def render_dev_reload_js() -> str:
    """The dev-only live-reload script ``synqt dev`` injects into the served bundle.

    It polls the reload token the file watcher bumps after every rebuild and reloads the
    page when the token changes, so a QML or contract edit shows up in the browser without
    a manual refresh. External and eval-free so the edge's strict Content-Security-Policy
    holds. The fetch stays same-origin (``connect-src 'self'``). Never emitted by
    ``synqt build``; only the watcher writes it into ``build/client/``.
    """
    return """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Injected by `synqt dev` only. Polls the reload token the file watcher bumps on every
// rebuild and reloads the page when it changes. External and eval-free so the edge's
// strict Content-Security-Policy holds. The fetch stays same-origin (connect-src 'self').
(function () {
    // Dev never has a shell cache of its own (build.client_cache is http here), but a
    // production build previously loaded from this origin (commonly localhost) leaves its
    // worker installed, and it would serve a cached shell over the dev build and silently
    // defeat the watcher below. Evict it.
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
