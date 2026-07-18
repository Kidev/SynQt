<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M2: Browser transport (WebSocketTransport)

The client's QIODevice adapter over `QWebSocket`, promoted from the M0 spike into the
framework client runtime ([`src/client/`](../../src/client)). QtRO does not speak
WebSocket, so the client wraps its `QWebSocket` in `SynQt::WebSocketTransport` and
hands it to the QtRO node with `addClientSideConnection()`. Binary messages only.

## Verdict

**PASS.** `tst_m2` acquires a host `Echo` Source through the adapter over a real local
plaintext WebSocket, then verifies both directions:

- a property change on the host (`source.setValue(7)`) reaches the Replica;
- a slot call from the client (`replica->poke(42)`) reaches the Source.

No registry; the connection is added manually on both ends.

## The adapter

`WebSocketTransport : QIODevice` over a `QWebSocket`:

- `isSequential()` is `true`.
- `bytesAvailable()` is the base plus the buffered incoming bytes.
- `open()` opens the underlying socket: with a `url()` set (the client case) it
  connects the socket to that url; with no url and an already-connected socket (the
  accepted-socket case) it just marks the device open. The device must be open before
  `addClientSideConnection()` / `addHostSideConnection()`; QtRO ignores a closed one.
- `readData` / `writeData` move bytes: outgoing as binary messages
  (`sendBinaryMessage`), incoming via `binaryMessageReceived` appended to the read
  buffer with `readyRead` emitted.
- `disconnected()` forwards the socket's disconnect.

Client wiring (from `tst_m2.cpp`):

```cpp
QWebSocket clientSocket;
SynQt::WebSocketTransport transport{&clientSocket};
transport.setUrl(QUrl{"ws://localhost:<port>"});
transport.open(QIODevice::ReadWrite);        // opens the socket
QRemoteObjectNode node;
node.addClientSideConnection(&transport);    // manual; no registry
node.setHeartbeatInterval(100);
auto *replica = node.acquire<EchoReplica>();
```

## How to run

```sh
tests/m2-transport/run-m2.sh
```

Builds the `SynQtClient` library and the test, then runs it via ctest.

## Notes

- One `WebSocketTransport` class serves both ends: the client (opens the socket to a
  url) and, in this test, the host (wraps each accepted socket, already connected).
  The M0 spike carried a separate `WebSocketIoDevice`; M2 is the framework version and
  the M0 spike remains as its own regression guard.
- `addClientSideConnection` requires an open device and drains any already-buffered
  bytes on attach, so opening the socket before adding the connection is race-free.
- Verified natively over a real loopback WebSocket. The same class links into the WASM
  client; M0 already proved the QtRO-over-WebSocket path end to end in real browsers.
