<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M3: Mesh transport (mutual TLS by default, opt-in local socket)

The service-to-service transports, in the framework service runtime
([`src/service/`](../../src/service), the `SynQtService` library). Mutual TLS is the
default on **every** mesh link; a same-host link is just mutual TLS bound to the
loopback address. The local socket is an explicit opt-in and is never selected
implicitly. This is the first milestone where the security default ships with the
link it protects.

## Verdict

**PASS.** Every clause of M3 acceptance, verified by `tst_m3`:

| clause | test |
|--------|------|
| two native nodes exchange a property and a slot over mutual TLS, each verified against the CA | `mutualTlsLoopbackExchange` |
| the owner reads the caller's entity name from the verified certificate subject | `mutualTlsLoopbackExchange` (`peer.entity == "beta"`, `authenticated`) |
| same-host link with no override comes up as mutual TLS on loopback | `mutualTlsLoopbackExchange` (binds `QHostAddress::LocalHost`) |
| a consumer presenting **no** certificate is rejected at the handshake | `missingCertificateRejected` |
| a consumer presenting a certificate from a **different CA** is rejected | `foreignCertificateRejected` |
| two native nodes exchange a property and a slot over an opted-in local socket | `localSocketExchange` |
| the local peer is trusted by colocation, not authenticated | `localSocketExchange` (`authenticated == false`) |
| the local socket file is restricted to the run-as user | `localSocketExchange` (permission check) |
| local is never selected implicitly | it requires an explicit `listenLocal()` / `connectLocal()` call |

## The transports

`MeshServer` (owner) and `MeshClient` (consumer) hand the accepted/opened `QIODevice`
to the caller (the entity runtime, M4) for `addHostSideConnection()` /
`addClientSideConnection()`. No registry.

**Mutual TLS (default).** `QSslServer` / `QSslSocket`, both configured with the
project CA (`setCaCertificates`) and `setPeerVerifyMode(VerifyPeer)`, presenting this
entity's certificate. The entity name is the certificate subject (CN): the server
reads the verified peer certificate's subject as the calling entity (`authenticated =
true`); the client verifies the owner's certificate identifies the expected owner
entity, not merely the address (`connectToHostEncrypted(addr, port, ownerEntity)` with
a `DNS:<entity>` SAN on the certificate). A peer with no certificate, or one from
another CA, fails the TLS handshake and never reaches `peerConnected()`.

**Local socket (opt-in).** `QLocalServer` / `QLocalSocket`. The socket file is
restricted to the run-as user (`QLocalServer::UserAccessOption`), and the peer's OS
credentials are checked through the native descriptor (`SO_PEERCRED` on Linux,
`getpeereid` on macOS). The OS identifies the *user*, not the *entity*: any same-user
process could present the configured name, so `MeshPeer::authenticated` is **false**;
colocation trust, not authentication. Authorization (M4/M7) must treat it accordingly.

## How to run

```sh
tests/m3-mesh/run-m3.sh
```

Builds `SynQtService` and the test, generating throwaway certificates at configure
time (project CA + `alpha`/`beta` entity certs with SANs + a foreign CA + a `rogue`
cert) into `build/m3-mesh/certs/`. These are **git-ignored and never committed**; no
production mesh CA key is created here.

## Notes / findings

- `QSslServer` with `VerifyPeer` rejects a bad client automatically: it does not emit
  `pendingConnectionAvailable`, only `errorOccurred` / `sslErrors`. In TLS 1.3 the
  rejected client may briefly compute an encrypted channel *before* the server's
  rejection arrives, so the authoritative check is server-side (no authenticated peer),
  corroborated by the client being dropped.
- A `QLocalSocket` connects **synchronously**, emitting `connected()` from within
  `connectToServer()`; the test checks the emission count (via `QTRY`) rather than
  waiting for a later signal.
- `MeshPeer` carries `authenticated` so callers can never conflate a certificate-
  verified entity with a colocation-trusted one.
