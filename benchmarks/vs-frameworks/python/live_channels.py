# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Django Channels: Django's ASGI application with the Channels consumer stack and group layer.

Same shape as ``node/live-bare.mjs``; everything but the server lives in ``measure.py``,
shared with ``live_fastapi.py``, so the gap between the two is the framework. The channel
layer is the in-memory one, publishing from the process that holds the sockets like every
other column; the README notes it.
"""

from __future__ import annotations

import asyncio
from typing import Any, List

import django
from channels.generic.websocket import AsyncWebsocketConsumer
from channels.routing import ProtocolTypeRouter, URLRouter
from django.conf import settings
from django.urls import path as url_path

import measure

# A minimal settings configuration: one endpoint, no models, views, templates, database or
# middleware.
if not settings.configured:
    settings.configure(
        DEBUG=False,
        ALLOWED_HOSTS=["*"],
        SECRET_KEY="benchmark-only-never-a-deployment",
        INSTALLED_APPS=["channels"],
        DATABASES={},
        USE_TZ=True,
    )
    django.setup()


class LiveConsumer(AsyncWebsocketConsumer):
    """One subscriber's consumer: it only joins and leaves the group the publisher writes to."""

    clients: List["LiveConsumer"] = []

    async def connect(self) -> None:
        await self.accept()
        LiveConsumer.clients.append(self)

    async def disconnect(self, code: int) -> None:
        if self in LiveConsumer.clients:
            LiveConsumer.clients.remove(self)


class Server:
    """Django's ASGI application on this process's event loop."""

    def __init__(self) -> None:
        self.url = ""
        self.endpoint: Any = None
        self.task: Any = None
        self.application = ProtocolTypeRouter({
            "websocket": URLRouter([url_path("live", LiveConsumer.as_asgi())]),
        })

    async def start(self) -> "Server":
        # Served by uvicorn rather than daphne, whose twisted reactor cannot share this
        # asyncio loop. The Channels consumer stack is what is measured; the README notes
        # it.
        from uvicorn import Config, Server as Uvicorn

        config = Config(self.application, host="127.0.0.1", port=0,
                        log_level="critical", access_log=False)
        self.endpoint = Uvicorn(config)
        self.task = asyncio.create_task(self.endpoint.serve())
        while not self.endpoint.started:
            await asyncio.sleep(0.01)
        port = self.endpoint.servers[0].sockets[0].getsockname()[1]
        self.url = f"ws://127.0.0.1:{port}/live"
        return self

    def connected(self) -> int:
        return len(LiveConsumer.clients)

    async def broadcast(self, frame: bytes) -> None:
        # Build the frame once and send the same bytes to every consumer.
        for client in list(LiveConsumer.clients):
            try:
                await client.send(bytes_data=frame)
            except Exception:
                pass

    async def close(self) -> None:
        for client in list(LiveConsumer.clients):
            try:
                await client.close()
            except Exception:
                pass
        LiveConsumer.clients.clear()
        self.endpoint.should_exit = True
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
        stack="python-channels",
        path="Django Channels consumers, ASGI WebSockets on uvicorn",
        args=args,
    )


if __name__ == "__main__":
    asyncio.run(main())
