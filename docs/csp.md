<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Content-Security-Policy and the browser-hardening headers

The web edge is the only entity a browser reaches, so it is where the browser-hardening
headers ship, on every page it serves. This page is the reference for what the edge sends,
how it **computes** the `Content-Security-Policy` (it never emits your configured string
raw), and how to widen it safely. It is written from the edge's `stampResponse` and
`computeCsp`, so it matches what actually goes on the wire. The security model those headers
are part of is in [security](security.md); the config keys are in
[project layout and config](project-layout-and-config.md).

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
upgrade before any login; its `SameSite`/`Secure` flags follow `origin_model` (see
[same-origin versus split-origin](#same-origin-versus-split-origin)).

## The default policy

`security.csp` defaults to a strict policy with no `'unsafe-inline'` and no `'unsafe-eval'`
in `script-src`:

```
default-src 'self'; connect-src 'self'; img-src 'self' data:;
style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval';
object-src 'none'; base-uri 'none'; frame-ancestors 'none'
```

- `script-src 'self' 'wasm-unsafe-eval'`; the WebAssembly client needs
  `'wasm-unsafe-eval'` to instantiate its module, but not `'unsafe-eval'`: SynQt's generated
  client shell reads `window.location` through Embind and builds with `-sDYNAMIC_EXECUTION=0`,
  so the Emscripten runtime never calls `eval()`. Keep `'unsafe-eval'` out.
- `style-src` allows `'unsafe-inline'` because Qt Quick's WebAssembly canvas sets inline
  styles; this is the one relaxation, and it is styles, not scripts.
- `object-src 'none'`, `base-uri 'none'`, `frame-ancestors 'none'` close off plugin
  embedding, `<base>` hijacking, and clickjacking.

## How the edge computes the policy

The edge does not send `security.csp` verbatim. `computeCsp` walks your directives and
adjusts three of them, so the policy stays correct as the deployment's origin, threading, and
bundle change without you hand-editing the string:

1. **`connect-src` gets the sync endpoint's explicit `wss://` origin appended.** Some
   browsers do not treat `'self'` as covering the WebSocket scheme, so the live data path
   would be blocked under a bare `connect-src 'self'`. The edge appends its own
   `wss://host:port` (or `ws://` in plaintext dev) every time. If you omit `connect-src`
   entirely, the edge adds `connect-src 'self' <wss-origin>` for you. **Do not hardcode the
   wss origin or port yourself**; the edge knows its bound port and appends it.

2. **`worker-src 'self' blob:` is added under cross-origin isolation.** When
   `cross_origin_isolation` is on (which `build.client_threads: multi` implies) the edge adds
   this directive if you did not write one, so the threaded client can start its pthread
   workers.

    The `blob:` half is a deliberate margin rather than a present need. Measured on
    2026-07-15 against the pinned toolchain (Qt 6.11.1, Emscripten 4.0.7), the loader
    spawns its workers from the same-origin `client.js`, not from `blob:` URLs: served
    under a strict `worker-src 'self'`, the multi-threaded client still reached cross-origin
    isolation and started its whole pthread pool in both Chromium and Firefox, with no CSP
    violation. The allowance stays because Emscripten's worker-spawning strategy is an
    implementation detail that has varied across versions, and because dropping it buys
    almost nothing: creating a `blob:` worker already requires script execution, which
    `script-src` governs. If you set your own `worker-src`, `'self'` alone is enough for
    this Qt and this emsdk.

3. **`script-src` gets the sha256 of each inline loader script.** The Qt WebAssembly loader
   the bundle ships has an inline bootstrap; rather than weaken the policy to
   `'unsafe-inline'`, the edge hashes each inline `<script>` in the served `index.html` and
   adds `'sha256-...'` to `script-src`, so the strict policy still runs the loader. (SynQt's own
   generated shell avoids inline handlers entirely; this covers a bundle that carries one.)

Everything else in your `security.csp` passes through unchanged.

## Cross-origin isolation

`cross_origin_isolation` gates three things together, and the edge keeps them consistent:
COOP `same-origin` + COEP `require-corp` (the two headers that put the page in an isolated
context so `SharedArrayBuffer` is available), plus the `worker-src 'self' blob:` CSP entry
above. Set it directly, or set `build.client_threads: multi` and it is implied; the
multi-threaded client cannot get `SharedArrayBuffer` without it, and validation enforces the
pairing. In this mode every subresource must be same-origin or carry
`Cross-Origin-Resource-Policy`/CORS, a direct consequence of COEP `require-corp`.

## Same-origin versus split-origin

`origin_model` changes the anti-hijacking surface, and the CSP and cookie follow it:

- **`same_origin`** (the default): the client is served from, and connects back to, the edge
  origin. `allowed_origins` is `[self]` (the edge origin), `connect-src 'self'` plus the
  appended wss origin is enough, and the session cookie is `SameSite=Lax` (`Secure` under
  TLS).
- **`split_origin`**: the client is served from a different origin than the sync endpoint
  (a CDN, say). `allowed_origins` must then list the client origin explicitly, and the
  session cookie is `SameSite=None; Secure` (or the subprotocol token when
  `session_transport: subprotocol`). The origin check at the wss upgrade (not the CSP) is
  the anti-CSWSH control in both models; the CSP `connect-src` still names the sync origin.

## Widening the policy safely

When an app genuinely needs a third-party origin (a font host, an image CDN, an external API
the *client* calls directly), edit `security.csp` and add the origin to the **specific**
directive, never to `default-src`, and never by reaching for a wildcard:

- Fonts -> `font-src`; images -> `img-src`; a directly-called API -> `connect-src` (it is
  merged with, not replaced by, the auto-appended wss origin).
- **Never** add `'unsafe-eval'` or `'unsafe-inline'` to `script-src`. If a bundle needs an
  inline script, let the edge hash it (serve it inline in `index.html`); if it needs eval,
  reconsider; the SynQt client does not.
- Keep `object-src 'none'`, `base-uri 'none'`, and `frame-ancestors 'none'`.
- Leave the wss origin out of what you write; the edge appends it.

`synqt check` fails a release build whose origin/isolation settings are inconsistent (for
example multi-threaded client without cross-origin isolation), and `synqt doctor` restates the
resulting header obligations. The web edge test suite asserts the computed policy; the appended
wss origin always, and COOP/COEP plus `worker-src 'self' blob:` under cross-origin isolation;
so a change that breaks the computation fails the build.
