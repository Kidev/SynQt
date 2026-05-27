<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The web edge

`WebEdge` ([`src/edge/webedge.*`](../../src/edge)) is the only internet-facing entity. On
`QHttpServer` it serves the client bundle with the browser-hardening headers, accepts the
browser's WebSocket through the upgrade verifier, which rejects a bad request before a
socket exists, and hands accepted sockets to a QtRO host so the browser can acquire the
edge's connect points. The public TLS, the computed CSP, COOP and COEP, the upgrade
checks, and the resource limits all ship here.

## Verdict

**PASS.** Every acceptance clause, verified by `tst_m5` over real TLS:

| clause | test |
|--------|------|
| serves the bundle with the CSP, wss origin in `connect-src` always | `bundleHeadersDefault` |
| COOP `same-origin` + COEP `require-corp` + `worker-src 'self' blob:` under cross-origin isolation | `bundleHeadersCrossOriginIsolated` |
| accepts an authorized upgrade and exposes its connect points | `authorizedUpgradeExposesConnectPoint` (acquires `greeting`, `value == 7`) |
| rejects an upgrade from a disallowed origin before a socket exists | `disallowedOriginRejectedBeforeSocket` |
| closes a connection that stalls its upgrade past `handshake_timeout_ms` | `stalledUpgradeClosed` |
| rejects an oversized frame | `oversizedFrameRejected` |

## The pipeline

`addAfterRequestHandler` stamps the headers, and the edge computes them instead of
copying them from config. The CSP always carries the sync endpoint's explicit
`wss://host:port` appended to `connect-src`. COOP, COEP and `worker-src 'self' blob:` are
emitted only under `crossOriginIsolation`. HSTS (on TLS), `X-Content-Type-Options: nosniff`
and `Referrer-Policy` are always present. The page load issues an httpOnly session cookie
(`SameSite=Lax` for same_origin, `SameSite=None; Secure` for split_origin), so the browser
has a credential to present at the upgrade.

The upgrade verifier (`addWebSocketUpgradeVerifier`) runs with the full request before a
socket exists and rejects on the first failure. It checks, in order, that the origin is in
`allowed_origins` (`self` means the edge origin), which is the primary anti-CSWSH control,
that the session cookie maps to a live session, the scope precondition (anonymous is
rejected exactly when `identity_required`), and the per-IP and global connection caps.

The edge enforces its own resource limits, because the QHttpServer path has none built
in. Each pending connection has a handshake timeout. The edge tracks each accepted socket
at `QSslServer::startedEncryptionHandshake`, keyed by `{peer address, port}`, and aborts
it when it does not present a complete upgrade request within `handshake_timeout_ms`. The
verifier cancels that timer by the same key. `setMaxAllowedIncomingMessageSize` and
`setMaxAllowedIncomingFrameSize` cap the frames on every accepted `QWebSocket`.

An accepted upgrade is wrapped in the host side of `SynQt::WebSocketTransport` and added
to a `QRemoteObjectHost`, so the browser acquires the edge's connect points, which are
Sources loaded from the edge's QML.

## How to run

```sh
tests/m5-webedge/run-m5.sh
```

It builds `SynQtEdge`, the library that carries Qt HttpServer, and the test, and
generates a throwaway localhost TLS server certificate at configure time into
`build/m5-webedge/certs/`. That is a public-link server certificate and not a mesh CA, and
it is git-ignored and never committed.

## What `aCdnEdgeServesNoFilesButStillMintsTheSession` is for

`public.serve_client: false` says a CDN delivers the bundle. Wiring that as "register
fewer routes" would have produced an app that loads perfectly and never connects. A
browser arriving from a CDN has never made a request to the edge, so it holds no session,
and the upgrade refuses a request that carries none.

So the test asserts all three parts together, and the third is the one that matters.
The edge serves no bundle file and no shell for a deep link, since both would be a staler
copy of what the CDN owns. Its client route answers a credentialed cross-origin request
with `204`, a session cookie, and an `Access-Control-Allow-Origin` echoing that exact
origin, and only when that origin is already allowed. The session it issued then passes
the wss upgrade and acquires a connect point.

The test is mutation-checked both ways. Registering the bundle routes unconditionally
fails the 404 half, and echoing any origin instead of an allowed one fails the refusal
half.

## What `theUpgradePathCannotNegotiateASubprotocol` is for

It is the only test here that pins a limitation instead of a behavior.
`security.session_transport: subprotocol` is documented as a config key and refused by
`synqt check`, and this is the evidence behind that refusal.

Carrying the session in `Sec-WebSocket-Protocol` needs the server to select one of the
offered subprotocols and echo it in the `101`. Qt 6.12 gives this path no way to say
which. `QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments, and the
`QWebSocketServer` that writes the response lives in `QAbstractHttpServerPrivate`, out of
reach of `setSupportedSubprotocols()`. The upgrade still completes and negotiates nothing,
which is what the test asserts.

Whether that matters turns out to depend on the engine, measured on 2026-07-28 against a
real generated edge:

| client | offered a subprotocol | result |
|--------|-----------------------|--------|
| Qt 6.11 `QWebSocket` | `synqt`, `synqt.session.<token>` | connects, `subprotocol()` empty |
| Chromium 149 | same | closed, code 1006 (`Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received`) |
| Firefox 151 | same | connects, `socket.protocol` empty |
| all three | nothing offered | connects |

An edge that works in one browser and not another is worse than one that refuses the
setting, which is why `synqt check` rejects the word. The test is a tripwire. If a later
Qt lets the verifier select a subprotocol, the test starts failing, and that failure says
the transport has become buildable.

## Notes / scope

- This suite uses a minimal proto-session, a cookie token in an in-memory store. The
  full `SessionManager` (expiry, revocation, rotation), the per-connect-point scope
  gating and the OAuth login have their own suites. The acceptance test runs over real
  TLS, and plaintext (`synqt dev`) is implemented too.
- The `WebSocketTransport` adapter is shared with the client runtime. The same source
  compiles into `SynQtTransport`, so the edge can wrap accepted browser sockets with no
  dependency from a service library on a client one.
- A blocking `waitForEncrypted` starves the same-process edge's event loop, so the stall
  test drives TLS asynchronously with `QTRY`.
- The full edge entity composes `WebEdge` on the client side with `EntityRuntime` on the
  mesh side, which is how it reaches a database. This suite tests `WebEdge` on its own.
- The `self` expansion in the CSP and the origin check uses the configured public host.
  An edge bound to `0.0.0.0` needs an explicit public origin in config.
