# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Python's fast async column: FastAPI on uvicorn, WebSockets, no ORM and no middleware.

The same shape as ``node/live-bare.mjs``, so a reader can put the two files
next to each other and see that the only difference is the runtime. Everything that is not
the server lives in ``measure.py``, which both Python columns share, so the difference
between this file and ``live_channels.py`` is only the framework.

Publisher and subscribers share one process and one event loop, which is the natural way
to write this in asyncio and is what the contract asks for. What it costs, and what
``measure.py`` keeps the subscriber callback short for, is that a slow subscriber would
delay the publisher on the same loop. That would be a self-inflicted queue rather than a
property of the stack.
"""

from __future__ import annotations

import asyncio
from typing import Any, List

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect

import measure


class Server:
    """One broadcast endpoint, running on this process's event loop."""

    def __init__(self) -> None:
        self.clients: List[WebSocket] = []
        self.app = FastAPI()
        self.server: uvicorn.Server | None = None
        self.task: asyncio.Task | None = None
        self.url = ""

        @self.app.websocket("/live")
        async def live(socket: WebSocket) -> None:  # noqa: ANN202 (FastAPI route)
            await socket.accept()
            self.clients.append(socket)
            try:
                # In this workload subscribers never send, so the read side exists only to
                # notice the close: without awaiting something the handler would return and
                # uvicorn would tear the connection down.
                while True:
                    await socket.receive_bytes()
            except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
                pass
            finally:
                if socket in self.clients:
                    self.clients.remove(socket)

    async def start(self) -> "Server":
        config = uvicorn.Config(self.app, host="127.0.0.1", port=0, log_level="critical",
                                access_log=False)
        self.server = uvicorn.Server(config)
        self.task = asyncio.create_task(self.server.serve())
        while not self.server.started:
            await asyncio.sleep(0.01)
        port = self.server.servers[0].sockets[0].getsockname()[1]
        self.url = f"ws://127.0.0.1:{port}/live"
        return self

    def connected(self) -> int:
        return len(self.clients)

    async def broadcast(self, frame: bytes) -> None:
        # Build the frame once, then hand the same bytes to every socket, as every other
        # column does. Writing per subscriber would measure the framing N times over.
        for socket in list(self.clients):
            try:
                await socket.send_bytes(frame)
            except Exception:
                pass

    async def close(self) -> None:
        for socket in list(self.clients):
            try:
                await socket.close()
            except Exception:
                pass
        self.clients.clear()
        if self.server is not None:
            self.server.should_exit = True
        if self.task is not None:
            try:
                await asyncio.wait_for(self.task, timeout=5)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                self.task.cancel()


async def connect(url: str) -> Any:
    import websockets

    return await websockets.connect(url, max_size=None, ping_interval=None)


async def main() -> None:
    args = measure.parse_args(measure.DEFAULTS)
    await measure.drive(
        start_server=lambda: Server().start(),
        connect=connect,
        stack="python-fastapi",
        path="FastAPI on uvicorn, WebSockets",
        args=args,
    )


if __name__ == "__main__":
    asyncio.run(main())
