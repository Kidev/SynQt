<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M3: Mesh transport (mutual TLS by default, opt-in local socket)

The service-to-service transports, in the framework service runtime
([`src/service/`](../../src/service), the `SynQtService` library). Mutual TLS is the
default on every mesh link, and a same-host link is mutual TLS bound to the loopback
address. The local socket is an explicit opt-in, and nothing selects it implicitly. The
security default ships with the link it protects.

## Verdict

**PASS.** Every clause of M3 acceptance, verified by `tst_m3`:

| clause | test |
|--------|------|
| two native nodes exchange a property and a slot over mutual TLS, each verified against the CA | `mutualTlsLoopbackExchange` |
| the owner reads the caller's entity name from the verified certificate subject | `mutualTlsLoopbackExchange` (`peer.entity == "beta"`, `authenticated`) |
| same-host link with no override comes up as mutual TLS on loopback | `mutualTlsLoopbackExchange` (binds `QHostAddress::LocalHost`) |
| a consumer presenting no certificate is rejected at the handshake | `missingCertificateRejected` |
| a consumer presenting a certificate from a different CA is rejected | `foreignCertificateRejected` |
| two native nodes exchange a property and a slot over an opted-in local socket | `localSocketExchange` |
| the local peer is trusted by colocation rather than authenticated | `localSocketExchange` (`authenticated == false`) |
| the local socket file is restricted to the run-as user | `localSocketExchange` (permission check) |
| local is never selected implicitly | it requires an explicit `listenLocal()` / `connectLocal()` call |

## The transports

`MeshServer` (owner) and `MeshClient` (consumer) hand the accepted or opened
`QIODevice` to the entity runtime, which passes it to `addHostSideConnection()` or
`addClientSideConnection()`. There is no registry.

Mutual TLS (the default). `QSslServer` and `QSslSocket`, both configured with the
project CA (`setCaCertificates`) and `setPeerVerifyMode(VerifyPeer)`, each presenting
this entity's certificate. The entity name is the certificate subject (CN). The server
reads the verified peer certificate's subject as the calling entity (`authenticated =
true`). The client checks that the owner's certificate identifies the expected owner
entity and not only the address, through
`connectToHostEncrypted(addr, port, ownerEntity)` against a `DNS:<entity>` SAN on the
certificate. A peer with no certificate, or one from another CA, fails the TLS handshake
and never reaches `peerConnected()`.

Local socket (opt-in). `QLocalServer` and `QLocalSocket`. The socket file is restricted
to the run-as user (`QLocalServer::UserAccessOption`), and the peer's OS credentials are
checked through the native descriptor (`SO_PEERCRED` on Linux, `getpeereid` on macOS).
The OS identifies the user, and it does not identify the entity. Any same-user process
could present the configured name, so `MeshPeer::authenticated` is false. That is
colocation trust, and the entity runtime's authorization has to treat it as such.

## How to run

```sh
tests/m3-mesh/run-m3.sh
```

It builds `SynQtService` and the test, and generates throwaway certificates at
configure time into `build/m3-mesh/certs/`: a project CA, `alpha` and `beta` entity certs
with SANs, a foreign CA, a `rogue` cert, and an `impostor` leaf that the project CA signs
for the address instead of for an entity. These are git-ignored and never committed, and
nothing here creates a production mesh CA key.

## Notes / findings

- `QSslServer` with `VerifyPeer` rejects a bad client on its own. It emits
  `errorOccurred` and `sslErrors`, and never `pendingConnectionAvailable`. Under TLS 1.3
  the rejected client may briefly compute an encrypted channel before the server's
  rejection arrives, so the authoritative check is the server side seeing no
  authenticated peer, corroborated by the client being dropped.
- A `QLocalSocket` connects synchronously and emits `connected()` from inside
  `connectToServer()`. The test checks the emission count with `QTRY` instead of waiting
  for a later signal.
- `MeshPeer` carries `authenticated`, so a caller cannot confuse a certificate-verified
  entity with a colocation-trusted one.
