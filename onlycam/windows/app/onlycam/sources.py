"""Frame sources: the phone browser page and the OnlyCam iOS application."""

from __future__ import annotations

import asyncio
import logging
import socket
import sys
import threading
import time
from collections.abc import Callable
from pathlib import Path

import requests
from aiohttp import WSMsgType, web

from . import certs, net

log = logging.getLogger(__name__)

JPEG_START = b"\xff\xd8"
JPEG_END = b"\xff\xd9"


def resource_dir() -> Path:
    """Directory holding the bundled web assets, both in dev and in the exe."""
    bundle = getattr(sys, "_MEIPASS", None)
    if bundle:
        return Path(bundle) / "web"
    return Path(__file__).resolve().parent / "web"


class FrameBus:
    """Holds the most recent JPEG frame produced by the active source."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._jpeg: bytes | None = None
        self._timestamp = 0.0
        self._counter = 0

    def publish(self, jpeg: bytes) -> None:
        with self._lock:
            self._jpeg = jpeg
            self._timestamp = time.monotonic()
            self._counter += 1

    def latest(self) -> tuple[bytes | None, float, int]:
        with self._lock:
            return self._jpeg, self._timestamp, self._counter

    def clear(self) -> None:
        with self._lock:
            self._jpeg = None


class Source:
    name = "source"

    def start(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError

    @property
    def status(self) -> str:
        return ""


class MjpegPullSource(Source):
    """Reads the MJPEG stream exposed by the OnlyCam iOS application."""

    name = "ios"

    def __init__(self, bus: FrameBus, url: str) -> None:
        self._bus = bus
        self._url = url
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._status = "Не запущено"

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="onlycam-mjpeg", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)
            self._thread = None
        self._status = "Остановлено"

    @property
    def status(self) -> str:
        return self._status

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._status = f"Подключаюсь к {self._url}"
                with requests.get(self._url, stream=True, timeout=10) as response:
                    response.raise_for_status()
                    self._status = "Поток с iPhone идёт"
                    buffer = b""
                    for chunk in response.iter_content(chunk_size=32768):
                        if self._stop.is_set():
                            return
                        if not chunk:
                            continue
                        buffer += chunk
                        while True:
                            start = buffer.find(JPEG_START)
                            end = buffer.find(JPEG_END, start + 2) if start != -1 else -1
                            if start == -1 or end == -1:
                                break
                            self._bus.publish(buffer[start:end + 2])
                            buffer = buffer[end + 2:]
                        if len(buffer) > 4 * 1024 * 1024:
                            buffer = b""
            except Exception as error:  # noqa: BLE001 - surfaced in the UI
                self._status = f"Нет связи: {error}"
                self._stop.wait(2)


class BrowserSource(Source):
    """HTTPS server that hands a capture page to the phone browser.

    The page grabs the camera with getUserMedia and pushes JPEG frames back
    over a WebSocket.
    """

    name = "browser"

    def __init__(self, bus: FrameBus, port: int = 8443,
                 on_client_change: Callable[[int], None] | None = None) -> None:
        self._bus = bus
        self._port = port
        self._on_client_change = on_client_change
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._runner: web.AppRunner | None = None
        self._clients = 0
        self._status = "Не запущено"
        self._ready = threading.Event()
        self._error: str | None = None

    @property
    def url(self) -> str:
        return f"https://{net.primary_ip()}:{self._port}/"

    @property
    def clients(self) -> int:
        return self._clients

    @property
    def status(self) -> str:
        return self._status

    def start(self) -> None:
        self._ready.clear()
        self._thread = threading.Thread(target=self._run, name="onlycam-http", daemon=True)
        self._thread.start()
        self._ready.wait(timeout=15)
        if self._error:
            raise RuntimeError(self._error)

    def stop(self) -> None:
        if self._loop and self._runner:
            future = asyncio.run_coroutine_threadsafe(self._runner.cleanup(), self._loop)
            try:
                future.result(timeout=5)
            except Exception:
                pass
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread:
            self._thread.join(timeout=5)
        self._thread = None
        self._runner = None
        self._loop = None
        self._clients = 0
        self._status = "Остановлено"

    def _run(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self._loop = loop
        try:
            loop.run_until_complete(self._serve())
            self._status = f"Жду телефон на {self.url}"
            self._ready.set()
            loop.run_forever()
        except Exception as error:  # noqa: BLE001 - surfaced in the UI
            self._error = str(error)
            self._status = f"Ошибка сервера: {error}"
            self._ready.set()
        finally:
            loop.close()

    async def _serve(self) -> None:
        app = web.Application(client_max_size=16 * 1024 * 1024)
        app.router.add_get("/", self._handle_index)
        app.router.add_get("/ws", self._handle_ws)
        app.router.add_static("/static", str(resource_dir()))

        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        context = certs.ssl_context(net.all_ips())
        site = web.TCPSite(self._runner, host="0.0.0.0", port=self._port, ssl_context=context)
        await site.start()

    async def _handle_index(self, request: web.Request) -> web.StreamResponse:
        page = resource_dir() / "index.html"
        return web.FileResponse(page, headers={"Cache-Control": "no-store"})

    async def _handle_ws(self, request: web.Request) -> web.WebSocketResponse:
        websocket = web.WebSocketResponse(max_msg_size=16 * 1024 * 1024, heartbeat=20)
        await websocket.prepare(request)
        self._clients += 1
        self._status = f"Телефон подключён ({self._clients})"
        if self._on_client_change:
            self._on_client_change(self._clients)
        try:
            async for message in websocket:
                if message.type == WSMsgType.BINARY:
                    self._bus.publish(message.data)
                elif message.type == WSMsgType.ERROR:
                    break
        finally:
            self._clients = max(0, self._clients - 1)
            self._status = (f"Телефон подключён ({self._clients})" if self._clients
                            else f"Жду телефон на {self.url}")
            if self._on_client_change:
                self._on_client_change(self._clients)
        return websocket


def port_is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("0.0.0.0", port))
            return True
        except OSError:
            return False
