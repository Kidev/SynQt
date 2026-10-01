<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Content-Security-Policy and the browser-hardening headers

The web edge is the only entity a browser reaches, so it sends the browser hardening
headers, on every page it serves. This page describes what the edge sends, how it computes
the `Content-Security-Policy` (it never sends your configured string as is), and how to
widen it safely. It follows the edge's `stampResponse` and `computeCsp`, so it matches what
goes on the wire. [Security](security.md) covers the security model around these headers,
and [project layout and config](project-layout-and-config.md) the config keys.

## What the edge sends

On every served page, `stampResponse` appends:

| Header | Value | When |
|--------|-------|------|
| `Content-Security-Policy` | the computed policy (below) | always |
| `Cross-Origin-Opener-Policy` | `same-origin` | `cross_origin_isolation: true` |
| `Cross-Origin-Embedder-Policy` | `require-corp` | `cross_origin_isolation: true` |
| `Strict-Transport-Security` | `max-age=63072000` | serving over TLS |
| `X-Content-Type-Options` | `nosniff` | always |
| `Referrer-Policy` | `same-origin` | always |
| `Set-Cookie` | the (anonymous) session cookie | on the client route only |

The `Set-Cookie` on the client route gives the browser a credential to present at the wss
upgrade before any login. Its `SameSite` and `Secure` flags follow `origin_model` (see
[same-origin versus split-origin](#same-origin-versus-split-origin)).

## The default policy

`security.csp` defaults to a strict policy with no `'unsafe-inline'` and no `'unsafe-eval'`
in `script-src`:

```
default-src 'self'; connect-src 'self'; img-src 'self' data:;
style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval';
object-src 'none'; base-uri 'none'; frame-ancestors 'none'
```

- **`script-src 'self' 'wasm-unsafe-eval'`.** The WebAssembly client needs
  `'wasm-unsafe-eval'` to instantiate its module, but not `'unsafe-eval'`: SynQt's generated
  client shell reads `window.location` through Embind and builds with
  `-sDYNAMIC_EXECUTION=0`, so the Emscripten runtime never calls `eval()`. Keep
  `'unsafe-eval'` out.
- **`style-src` allows `'unsafe-inline'`,** because Qt Quick's WebAssembly canvas sets
  inline styles. This is the only relaxation, and it covers styles only.
- **`object-src 'none'`, `base-uri 'none'`, `frame-ancestors 'none'`** block plugin
  embedding, `<base>` hijacking and clickjacking.

## How the edge computes the policy

The edge rewrites `security.csp` before sending it. `computeCsp` walks your directives and
adjusts three of them, so the policy stays correct as the deployment's origin, threading
and bundle change, without you editing the string:

1. **`connect-src` gets the sync endpoint's explicit `wss://` origin.** Some browsers do not
   treat `'self'` as covering the WebSocket scheme, so a bare `connect-src 'self'` would
   block the live data path. The edge always appends its own `wss://host:port` (or `ws://`
   in plaintext development). If you omit `connect-src`, the edge adds
   `connect-src 'self' <wss-origin>`. Do not hardcode the wss origin or port; the edge knows
   its bound port.

2. **`worker-src 'self' blob:` is added under cross origin isolation.** When
   `cross_origin_isolation` is on (which `build.client_threads: multi` implies), the edge
   adds this directive unless you wrote one, so the threaded client can start its pthread
   workers.

    `blob:` is a margin for a future toolchain; the current one does not need it. On the
    toolchain it was measured with (Qt 6.11.1 and Emscripten 4.0.7, before the pin moved to
    6.12.0), the loader spawns its workers from the same origin `client.js`, not from
    `blob:` URLs. The
    [multi threaded proof](https://github.com/Kidev/SynQt/blob/main/tests/transport-spike/verify/verify-mt.mjs)
    serves its threaded bundle under the strict policy, `worker-src 'self'` with no
    `blob:`, on every run and in every engine it can launch, and reports each engine's
    policy violations by directive. All three supported engines have been measured, and
    none needs `blob:`: Chromium and Firefox since 2026-07-15, and WebKit, the last, on
    2026-07-31 on macOS 15.7.8 (WebKit 26.5). Each reached cross origin isolation, got
    `SharedArrayBuffer`, started its whole pthread pool and logged no CSP violation. A
    future toolchain that needs `blob:` will show up as a failure in that run, not as a
    mystery in production. The allowance stays because Emscripten's way of spawning
    workers has changed across versions, and removing it gains almost nothing: creating a
    `blob:` worker already requires running script, which `script-src` governs. If you
    set your own `worker-src`, `'self'` alone is enough for this Qt and this emsdk in
    every engine SynQt targets.

3. **`script-src` gets the sha256 of each inline loader script.** The Qt WebAssembly loader
   in the bundle has an inline bootstrap. Instead of weakening the policy to
   `'unsafe-inline'`, the edge hashes each inline `<script>` in the served `index.html` and
   adds `'sha256-...'` to `script-src`, so the loader runs under the strict policy. (SynQt's
   generated shell has no inline handlers; this covers a bundle that does.)

Everything else in your `security.csp` passes through unchanged.

## Cross-origin isolation

`cross_origin_isolation` controls three things together, and the edge keeps them
consistent: COOP `same-origin` and COEP `require-corp` (the two headers that isolate the
page so `SharedArrayBuffer` is available), plus the `worker-src 'self' blob:` CSP entry
above. Set it directly, or set `build.client_threads: multi`, which implies it. The multi
threaded client cannot get `SharedArrayBuffer` without it, so the build turns it on for a
threaded client, and `synqt check` warns when the configuration said `false`. In this
mode, every subresource must be same origin or carry `Cross-Origin-Resource-Policy` or
CORS headers, a consequence of COEP `require-corp`.

## Same-origin versus split-origin

`origin_model` changes the anti-hijacking surface, and the CSP and cookie follow it:

- **Same origin** (no `origin_model` declared): the client is served from the edge origin
  and connects back to it. `allowed_origins` is `[self]` (the edge origin),
  `connect-src 'self'` plus the appended wss origin is enough, and the session cookie is
  `SameSite=Lax` (and `Secure` under TLS).
- **`split_origin`** (deprecated, written by hand, never scaffolded): the client is served
  from a different origin than the sync endpoint, such as a CDN. `allowed_origins` must list
  the client origin, and the session cookie is `SameSite=None; Secure`, a third party
  cookie, which browsers are phasing out. Read
  [serving the client from another origin](project-layout-and-config.md#serving-the-client-from-another-origin)
  before choosing it.

In both models, the origin check at the wss upgrade, not the CSP, defends against cross
site WebSocket hijacking. The CSP's `connect-src` still names the sync origin.

## Widening the policy safely

When an app really needs a third party origin (a font host, an image CDN, an external API
the *client* calls directly), add the origin to the specific directive in `security.csp`,
never to `default-src`, and never as a wildcard:

- **Put each origin in its directive:** fonts in `font-src`, images in `img-src`, a directly
  called API in `connect-src` (the edge merges its wss origin into it instead of replacing
  it).
- **Never add `'unsafe-eval'` or `'unsafe-inline'` to `script-src`.** If a bundle needs an
  inline script, let the edge hash it (serve it inline in `index.html`). If it needs eval,
  reconsider; the SynQt client does not.
- **Keep `object-src 'none'`, `base-uri 'none'` and `frame-ancestors 'none'`.**
- **Leave out the wss origin.** The edge appends it.

`synqt check` refuses inconsistent origin settings (an edge whose client comes from
another origin while `allowed_origins` names nothing but `self`) and warns when it
overrides the isolation a threaded client needs, and `synqt doctor` lists the resulting
header obligations. The web edge test suite checks the computed policy (the appended wss
origin always, plus COOP, COEP and `worker-src 'self' blob:` under cross origin
isolation), so a change that breaks the computation fails the build.

## Remote pages need no CSP change

A [remote page](remote-pages.md) is QML the web edge delivers at run time, but it needs no
CSP change. It arrives over the `wss` link as data, and the QML engine inside the
WebAssembly module parses it. It never reaches the browser's JavaScript engine, never
becomes a `<script>`, and never calls `eval()`. `script-src` governs only what the
browser's JavaScript engine runs, which never sees a remote page, so `'unsafe-inline'` and
`'unsafe-eval'` stay out and the strict policy is the same with or without remote pages.
The client bounds a delivered page with the
[palette](remote-pages.md#the-palette-what-a-delivered-page-may-import), the set of QML
modules it may import, not with the CSP.
