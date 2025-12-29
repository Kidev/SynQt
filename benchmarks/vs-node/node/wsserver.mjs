// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A WebSocket server in Node built-ins alone: node:http for the upgrade and node:crypto for
// the handshake accept. No dependency, which is the point of the "bare" column.
//
// Server-to-client framing only, which is all this workload needs and is the simple half of
// RFC 6455: no masking (a server must not mask), and one frame per message. Incoming frames
// are parsed only far enough to notice a close, because the subscribers in this benchmark
// never send anything.
//
// This is not a WebSocket library and is not trying to be. It exists so the bare column
// measures Node's own I/O rather than a package's, and it is deliberately small enough to
// read in one sitting and confirm that.

import { createHash } from "node:crypto";
import { createServer } from "node:http";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// One outgoing binary frame. Lengths above 125 take the extended forms, which a 256-byte
/// payload already does, so all three cases are exercised by an ordinary run.
export function encodeBinaryFrame(payload) {
    const length = payload.length;
    let header;
    if (length < 126) {
        header = Buffer.alloc(2);
        header[1] = length;
    } else if (length < 65536) {
        header = Buffer.alloc(4);
        header[1] = 126;
        header.writeUInt16BE(length, 2);
    } else {
        header = Buffer.alloc(10);
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(length), 2);
    }
    header[0] = 0x82; // FIN + binary
    return Buffer.concat([header, payload]);
}

/// Start a broadcast server. `onCount` is called whenever the client count changes, so a
/// caller can wait for every subscriber to be connected before it starts measuring.
export function startBroadcastServer({ port = 0, onCount = () => {} } = {}) {
    const clients = new Set();

    const http = createServer((_request, response) => {
        response.writeHead(426);
        response.end("upgrade required");
    });

    http.on("upgrade", (request, socket) => {
        const key = request.headers["sec-websocket-key"];
        if (!key) {
            socket.destroy();
            return;
        }
        const accept = createHash("sha1").update(key + GUID).digest("base64");
        socket.write(
            "HTTP/1.1 101 Switching Protocols\r\n" +
            "Upgrade: websocket\r\n" +
            "Connection: Upgrade\r\n" +
            `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
        // Nagle would batch small frames and turn a latency measurement into a measurement
        // of the batching timer. The Qt side does not batch either.
        socket.setNoDelay(true);
        clients.add(socket);
        onCount(clients.size);

        const drop = () => {
            if (clients.delete(socket)) {
                onCount(clients.size);
            }
        };
        socket.on("close", drop);
        socket.on("error", drop);
        // Read and discard: the subscribers never send, but a close frame arrives as data
        // and an unread socket would apply backpressure to it.
        socket.on("data", () => {});
    });

    return new Promise((resolve) => {
        http.listen(port, "127.0.0.1", () => {
            resolve({
                port: http.address().port,
                clientCount: () => clients.size,
                broadcast(payload) {
                    const frame = encodeBinaryFrame(payload);
                    for (const socket of clients) {
                        socket.write(frame);
                    }
                    return clients.size;
                },
                close() {
                    for (const socket of clients) {
                        socket.destroy();
                    }
                    clients.clear();
                    http.close();
                },
            });
        });
    });
}
