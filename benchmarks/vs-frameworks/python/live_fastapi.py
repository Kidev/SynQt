# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""FastAPI on uvicorn, WebSockets, no ORM and no middleware.

Same shape as ``node/live-bare.mjs``; everything but the server lives in ``measure.py``,
shared with ``live_channels.py``. Publisher and subscribers share one event loop, which is
why ``measure.py`` keeps the subscriber callback short.
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
        async def live(socket: WebSocket) -> None: # noqa: ANN202 (FastAPI route)
            await socket.accept()
            self.clients.append(socket)
            try:
                # Subscribers never send; awaiting the read only detects the close.
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
        # Build the frame once and send the same bytes to every socket.
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
