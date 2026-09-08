import io
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from PIL import Image

from onlycam.sources import BrowserSource, FrameBus, MjpegPullSource, port_is_free


def _jpeg(color=(0, 128, 255)) -> bytes:
    buffer = io.BytesIO()
    Image.new("RGB", (32, 24), color).save(buffer, format="JPEG")
    return buffer.getvalue()


class _MjpegHandler(BaseHTTPRequestHandler):
    frame = b""

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        for _ in range(20):
            self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n")
            self.wfile.write(self.frame)
            self.wfile.write(b"\r\n")
            self.wfile.flush()
            time.sleep(0.05)

    def log_message(self, *args):
        pass


def test_mjpeg_source_reads_frames_from_the_ios_app():
    frame = _jpeg()
    _MjpegHandler.frame = frame
    server = ThreadingHTTPServer(("127.0.0.1", 0), _MjpegHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    bus = FrameBus()
    source = MjpegPullSource(bus, f"http://127.0.0.1:{server.server_port}/stream")
    try:
        source.start()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and bus.latest()[0] is None:
            time.sleep(0.05)
        assert bus.latest()[0] == frame
        assert source.status == "Поток с iPhone идёт"
    finally:
        source.stop()
        server.shutdown()


def test_mjpeg_source_reports_a_dead_phone():
    source = MjpegPullSource(FrameBus(), "http://127.0.0.1:9/stream")
    try:
        source.start()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not source.status.startswith("Нет связи"):
            time.sleep(0.05)
        assert source.status.startswith("Нет связи")
    finally:
        source.stop()


def test_browser_source_serves_the_page_and_accepts_frames():
    aiohttp = pytest.importorskip("aiohttp")
    import asyncio

    port = 8443 if port_is_free(8443) else 18443
    bus = FrameBus()
    source = BrowserSource(bus, port=port)
    source.start()
    frame = _jpeg((10, 200, 10))

    async def phone():
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        async with aiohttp.ClientSession() as session:
            async with session.get(f"https://127.0.0.1:{port}/", ssl=context) as response:
                assert response.status == 200
                assert "getUserMedia" in await response.text()
            async with session.ws_connect(f"wss://127.0.0.1:{port}/ws", ssl=context) as socket:
                await socket.send_bytes(frame)
                await asyncio.sleep(0.3)
                assert source.clients == 1

    try:
        asyncio.run(phone())
        assert bus.latest()[0] == frame
    finally:
        source.stop()
