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

## The unit cases (`tst_wstransport.cpp`)

The acceptance test above cannot prove the device contract underneath it. QtRO reads
whole frames as soon as they arrive, so it never takes a short read, never sends a
multi-megabyte message, and never outlives its socket. Those are the paths a change to
the read buffer would break, and they would break quietly: small messages would keep
working while large ones lost bytes. `tst_wstransport` is the adapter on its own, over a
real loopback pair, with no QtRO node on either end:

- **Framing**: one binary message per write, one `readyRead` per message, and a byte
  stream on the far side (QtRO frames its own protocol inside that stream).
- **Partial reads**: a consumer that takes less than has arrived keeps the remainder in
  order, and `bytesAvailable()` keeps telling it the truth. Run both buffered (how QtRO
  opens the device) and unbuffered, which is what puts a short read on the adapter's own
  `readData` instead of on the QIODevice buffer above it. A second case reads part of the
  buffer, receives more, and reads the rest, so a front erase and a back append meet over
  the same unread bytes.
- **Large messages**: 4 MiB, whole and byte-exact; and 200 messages back to back in both
  directions at once, still in order.
- **Drain cost**: 16 MiB buffered and read out 1 KiB at a time, on a clock. This is the
  one case that measures rather than compares, and it guards something the code relies on
  without being promised it. `readData` erases from the front of the read buffer on every
  call, which looks quadratic and is not: Qt 6's `QArrayDataPointer::erase` advances the
  begin pointer for a range starting at `begin()` instead of moving the remainder, and the
  next append that needs room reclaims the gap. `QByteArray::remove()` documents only that
  capacity is preserved, so the property is real but unpromised. It measures 1 to 2 ms
  against a 2000 ms budget; a genuinely quadratic drain would move about 128 GiB and take
  tens of seconds, and the loop gives up at the budget so the failure is fast.
- **The read-buffer ceiling**: the buffer is the one place in the transport where a remote
  party decides how much memory is allocated, and nothing drains it but a consumer that
  calls `read()`. `setReadBufferLimit()` caps it (64 MiB by default, on without being asked
  for; the edge tightens it to four times `max_message_bytes` per connection, since a
  browser's frames are already capped one at a time and this is what caps their sum). Past
  the ceiling the device discards the buffer and closes, rather than dropping a message:
  QtRO is framed, so a stream missing a message in the middle is desynchronized, not
  degraded, and a dropped connection is something the client's reconnect path already
  handles. Tested at the boundary in both directions, because a cap that fires one frame
  early looks like it works while killing connections that did nothing wrong.
- **Giving the memory back**: `remove()` preserves capacity, so a connection that once
  carried one large frame would hold that allocation until it closed. The device releases
  it when the buffer empties, and never otherwise, so the release cannot copy anything.
  Capacity is not visible from outside the class and widening the API to see it would be
  testing through a hole cut for the test, so the case measures the process instead: 48 MiB
  is well past the allocator's mmap threshold, so both keeping it and returning it show up
  in `/proc/self/statm`. It checks that the buffer is visible arriving before concluding
  anything about it leaving. Linux only, since that is where the measurement lives; the
  behaviour is not platform specific.
- **Close handling**: `close()` closes the socket under it and both ends learn of it;
  bytes already buffered survive the peer disconnecting; and a socket destroyed before
  the device leaves the device answering safely rather than reaching through a dangling
  pointer.

Worth knowing when reading these: the acceptance test passes against a transport that
throws away everything a short read did not consume. The buffered partial-read row passes
too. The unbuffered row and the large-message case are what catch it.

## How to run

```sh
tests/m2-transport/run-m2.sh
```

Builds the `SynQtClient` library and both tests, then runs them via ctest: `m2` (the
acceptance path) and `wstransport` (the unit cases).

## Notes

- One `WebSocketTransport` class serves both ends: the client (opens the socket to a
  url) and, in this test, the host (wraps each accepted socket, already connected).
  The M0 spike carried a separate `WebSocketIoDevice`; M2 is the framework version and
  the M0 spike remains as its own regression guard.
- `addClientSideConnection` requires an open device and drains any already-buffered
  bytes on attach, so opening the socket before adding the connection is race-free.
- Verified natively over a real loopback WebSocket. The same class links into the WASM
  client; M0 already proved the QtRO-over-WebSocket path end to end in real browsers.
