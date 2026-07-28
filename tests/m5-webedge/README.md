<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M5: The web edge

`WebEdge` ([`src/service/webedge.*`](../../src/service)) is the only internet-facing
entity. On `QHttpServer` it serves the client bundle with the browser-hardening
headers, accepts the browser's WebSocket through the upgrade verifier (**rejecting bad
requests before a socket exists**), and hands accepted sockets to a QtRO host so the
browser can acquire the edge's connect points. The public TLS, the computed
CSP/COOP/COEP, the upgrade checks, and the resource limits all ship here.

## Verdict

**PASS.** Every clause of the M5 build guide, verified by `tst_m5` **over real TLS**:

| clause | test |
|--------|------|
| serves the bundle with the CSP, wss origin in `connect-src` always | `bundleHeadersDefault` |
| COOP `same-origin` + COEP `require-corp` + `worker-src 'self' blob:` under cross-origin isolation | `bundleHeadersCrossOriginIsolated` |
| accepts an authorized upgrade and exposes its connect points | `authorizedUpgradeExposesConnectPoint` (acquires `greeting`, `value == 7`) |
| rejects an upgrade from a disallowed origin **before a socket exists** | `disallowedOriginRejectedBeforeSocket` |
| closes a connection that stalls its upgrade past `handshake_timeout_ms` | `stalledUpgradeClosed` |
| rejects an oversized frame | `oversizedFrameRejected` |

## The pipeline

**Headers** (stamped via `addAfterRequestHandler`, computed not raw): CSP with the
sync endpoint's explicit `wss://host:port` appended to `connect-src` **always**;
COOP/COEP and `worker-src 'self' blob:` **only** under `crossOriginIsolation`; HSTS
(TLS), `X-Content-Type-Options: nosniff`, `Referrer-Policy`. An httpOnly session cookie
(`SameSite=Lax` for same_origin, `SameSite=None; Secure` for split_origin) is issued on
the page load so the browser has a credential to present at the upgrade.

**Upgrade verifier** (`addWebSocketUpgradeVerifier`, run with the full request before a
socket exists; reject on first failure): (1) origin in `allowed_origins` (`self` => edge
origin); the primary anti-CSWSH control; (2) session cookie maps to a live session;
(3) scope precondition (anonymous rejected iff `identity_required`); (4) per-IP and
global connection caps.

**Self-enforced resource limits** (the QHttpServer path has none built in): a
**handshake timeout** per pending connection; each accepted socket is tracked at
`QSslServer::startedEncryptionHandshake` (keyed by `{peer address, port}`) and aborted
if it does not present a complete upgrade request within `handshake_timeout_ms`; the
verifier cancels that timer by the same key. Frames are capped with
`setMaxAllowedIncomingMessageSize/FrameSize` on every accepted `QWebSocket`.

**Accepted upgrades** are wrapped in the M2 `SynQt::WebSocketTransport` (host-side) and
added to a `QRemoteObjectHost`, so the browser acquires the edge's connect points
(Sources loaded from the edge's QML, as in M4).

## How to run

```sh
tests/m5-webedge/run-m5.sh
```

Builds `SynQtService` (now with Qt HttpServer) and the test, generating a throwaway
localhost TLS server cert at configure time into `build/m5-webedge/certs/`; a
public-link server cert (not a mesh CA), git-ignored and never committed.

## What `aCdnEdgeServesNoFilesButStillMintsTheSession` is for

`public.serve_client: false` says a CDN delivers the bundle. Wiring that as "register
fewer routes" would have produced an app that loads perfectly and never connects, because
a browser arriving from a CDN has never made a request to the edge and therefore holds no
session, and the upgrade refuses a request that carries none.

So the test asserts all three parts together, and the third is the one that matters: the
edge serves no bundle file and no shell for a deep link (both would be a staler copy of
what the CDN owns), its client route answers a credentialed cross origin request with
`204`, a session cookie, and an `Access-Control-Allow-Origin` echoing that exact origin
only when it is already allowed, and **the session it issued then passes the wss upgrade
and acquires a connect point**.

Mutation-checked both ways: registering the bundle routes unconditionally fails the 404
half, and echoing any origin instead of an allowed one fails the refusal half.

## What `theUpgradePathCannotNegotiateASubprotocol` is for

It is the only test here that pins a limitation rather than a behavior, so it is worth
saying what it buys. `security.session_transport: subprotocol` is documented as a config
key and refused by `synqt check`, and this is the evidence behind that refusal.

Carrying the session in `Sec-WebSocket-Protocol` needs the server to select one of the
offered subprotocols and echo it in the `101`. Qt 6.11 gives this path no way to say which:
`QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments, and the
`QWebSocketServer` that writes the response lives in `QAbstractHttpServerPrivate`, out of
reach of `setSupportedSubprotocols()`. The upgrade still completes, negotiating nothing,
which is what the test asserts.

Whether that matters turns out to depend on the engine, measured on 2026-07-28 against a
real generated edge:

| client | offered a subprotocol | result |
|--------|-----------------------|--------|
| Qt 6.11 `QWebSocket` | `synqt`, `synqt.session.<token>` | connects, `subprotocol()` empty |
| Chromium 149 | same | **closed, code 1006** (`Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received`) |
| Firefox 151 | same | connects, `socket.protocol` empty |
| all three | nothing offered | connects |

An edge that works in one browser and not another is worse than one that refuses the
setting, which is why the word is rejected at `synqt check`. The test is a tripwire: if a
later Qt lets the verifier select a subprotocol, it starts failing, and that failure says
the transport has become buildable.

## Notes / scope

- Decisions: M5 uses a **minimal proto-session** (a cookie token in an in-memory store);
  the full `SessionManager` (expiry, revocation, rotation) and
  per-connect-point **scope gating** land in **M7**, and OAuth login in **M8**. The
  acceptance test runs over **real TLS**; plaintext (`synqt dev`) is implemented too.
- The `WebSocketTransport` adapter is shared with the client runtime; the same source
  is compiled into `SynQtService` so the edge can wrap accepted browser sockets without
  a service->client library dependency.
- In-process test trap worth noting: a blocking `waitForEncrypted` starves the
  same-process edge's event loop, so the stall test drives TLS asynchronously (`QTRY`).
- The full edge *entity* composes `WebEdge` (client side) with M4's `EntityRuntime`
  (mesh side, e.g. reaching a database). M5 tests `WebEdge` standalone.
- Production note: the CSP/origin "self" expansion uses the configured public host; an
  edge bound to `0.0.0.0` needs an explicit public origin in config (an M10 concern).
