<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The browser transport (WebSocketTransport)

The client's QIODevice adapter over `QWebSocket`, promoted from the transport spike into
the framework client runtime ([`src/client/`](../../src/client)). QtRO does not speak
WebSocket, so the client wraps its `QWebSocket` in `SynQt::WebSocketTransport` and hands it
to the QtRO node with `addClientSideConnection()`. Binary messages only.

## Verdict

**PASS.** `tst_m2` acquires a host `Echo` Source through the adapter over a real local
plaintext WebSocket, and then verifies both directions:

- a property change on the host (`source.setValue(7)`) reaches the Replica
- a slot call from the client (`replica->poke(42)`) reaches the Source

There is no registry. Both ends add the connection by hand.

## The adapter

`WebSocketTransport : QIODevice` over a `QWebSocket`:

- `isSequential()` is `true`.
- `bytesAvailable()` is the base plus the buffered incoming bytes.
- `open()` opens the underlying socket. With a `url()` set, which is the client case, it
  connects the socket to that url. With no url and an already-connected socket, which is
  the accepted-socket case, it marks the device open. The device has to be open before
  `addClientSideConnection()` or `addHostSideConnection()`, because QtRO ignores a closed
  one.
- `readData` and `writeData` move bytes. Outgoing bytes go as binary messages through
  `sendBinaryMessage`, and incoming ones arrive on `binaryMessageReceived`, are appended
  to the read buffer, and raise `readyRead`.
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
multi-megabyte message, and never outlives its socket. Those are the paths a change to the
read buffer would break, and they would break quietly: small messages would keep working
while large ones lost bytes. `tst_wstransport` is the adapter on its own, over a real
loopback pair, with no QtRO node on either end:

- Framing. One binary message per write, one `readyRead` per message, and a byte
  stream on the far side (QtRO frames its own protocol inside that stream).
- Partial reads. A consumer that takes less than has arrived keeps the remainder in
  order, and `bytesAvailable()` stays accurate. This runs both buffered, which is how QtRO
  opens the device, and unbuffered, which puts a short read on the adapter's own `readData`
  instead of on the QIODevice buffer above it. A second case reads part of the buffer,
  receives more, and reads the rest, so a front erase and a back append meet over the same
  unread bytes.
- Large messages. 4 MiB, whole and byte-exact, and 200 messages back to back in both
  directions at once, still in order.
- Drain cost. 16 MiB buffered and read out 1 KiB at a time, on a clock. This is the one
  case that measures instead of comparing, and it guards something the code relies on
  without being promised it. `readData` erases from the front of the read buffer on every
  call, which looks quadratic and is not. Qt 6's `QArrayDataPointer::erase` advances the
  begin pointer for a range starting at `begin()` instead of moving the remainder, and the
  next append that needs room reclaims the gap. `QByteArray::remove()` documents only that
  capacity is preserved, so the property is real and unpromised. It measures 1 to 2 ms
  against a 2000 ms budget. A quadratic drain would move about 128 GiB and take tens of
  seconds, and the loop gives up at the budget, so the failure is fast.
- The read-buffer ceiling. The buffer is the one place in the transport where a remote
  party decides how much memory is allocated, and only a consumer calling `read()` drains
  it. `setReadBufferLimit()` caps it at 64 MiB by default, without being asked for, and
  the edge tightens it to four times `max_message_bytes` per connection. A browser's frames
  are already capped one at a time, and this caps their sum. Past the ceiling the device
  discards the buffer and closes, and it never drops a single message. QtRO is framed, so a
  stream missing a message in the middle is desynchronized and not merely degraded, while
  a dropped connection is something the client's reconnect path already handles. This is
  tested at the boundary in both directions, because a cap that fires one frame early looks
  like it works while killing connections that did nothing wrong.
- Giving the memory back. `remove()` preserves capacity, so a connection that once
  carried one large frame would hold that allocation until it closed. The device releases
  it when the buffer empties, and never otherwise, so the release cannot copy anything.
  Capacity is not visible from outside the class, and widening the API to see it would be
  testing through a hole cut for the test, so the case measures the process instead. 48 MiB
  is well past the allocator's mmap threshold, so both keeping it and returning it show up
  in `/proc/self/statm`. The case checks that the buffer is visible arriving before it
  concludes anything about it leaving. It runs on Linux only, since that is where the
  measurement lives, and the behaviour itself is not platform specific.
- Close handling. `close()` closes the socket under it and both ends learn of it,
  bytes already buffered survive the peer disconnecting, and a socket destroyed before
  the device leaves the device answering safely rather than reaching through a dangling
  pointer.

The acceptance test passes against a transport that throws away everything a short read
did not consume, and so does the buffered partial-read row. The unbuffered row and the
large-message case are what catch it.

## The split cases (`tst_threadedsocket.cpp`)

The same adapter, cut in half. The `QIODevice` QtRO writes into stays on the thread that
owns the host, and the `QWebSocket` under it lives on an IO thread. That is what a web
edge's [`threads:`](../../docs/deploying.md#running-one-edge-on-more-than-one-core) key
does to every accepted browser socket, and it is the only place in the framework where one
connection spans two threads.

The cases are about the split itself. The two halves are on different threads, bytes
cross in both directions, nothing is reordered on the way, a batch respects the message
ceiling the browser end is held to, and destroying the device puts its socket down on the
socket's own thread instead of from this one.

One of them is about cost and not correctness.
`aFanOutCrossesOncePerSocketThreadRatherThanOncePerConnection` writes to eight split
connections in one pass and asserts that one queued call crosses to their shared IO thread.
Crossing once per connection is what stops a threaded edge gaining throughput after two
cores. A queued call costs about a microsecond, which is nothing against what delivering to
a connection costs and everything against it a hundred times over, so the thread holding
the Sources spends its pass posting instead of serialising. Grouping them is worth 14% to
18% of saturating throughput at two, four and eight threads, and a fifth off the
propagation p50. The case counts the crossings by filtering the IO thread's event
dispatcher, which is the object every crossing is addressed to. It reports eight the moment
the grouping is undone.

## How to run

```sh
tests/m2-transport/run-m2.sh
```

It builds the `SynQtClient` library and every test, then runs them under ctest: `m2`,
the acceptance path; `wstransport`, the unit cases; `threadedsocket`, the split cases;
`iothreads`, the pool that hands the threads out; and `proxypolicy`, which proxy a client
is allowed to dial through.

## Notes

- One `WebSocketTransport` class serves both ends: the client, which opens the socket to
  a url, and, in this test, the host, which wraps each accepted and already-connected
  socket. The transport spike carries a separate `WebSocketIoDevice`. This is the framework
  version, and the spike remains as its own regression guard.
- `addClientSideConnection` requires an open device and drains any already-buffered bytes
  on attach, so opening the socket before adding the connection has no race in it.
- This is verified natively over a real loopback WebSocket. The same class links into the
  WASM client, and the transport spike proved the QtRO-over-WebSocket path end to end in
  real browsers.
