"""Decodes incoming JPEG frames and publishes them to the virtual camera."""

from __future__ import annotations

import io
import logging
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np
from PIL import Image, ImageOps

from .framebuffer import SharedFrameWriter
from .sources import FrameBus

log = logging.getLogger(__name__)

STALE_AFTER_SECONDS = 5.0


@dataclass
class Stats:
    input_fps: float = 0.0
    output_fps: float = 0.0
    size: tuple[int, int] = (0, 0)
    consumers: int = 0
    last_error: str = ""
    lock: threading.Lock = field(default_factory=threading.Lock, repr=False)


class Publisher(threading.Thread):
    def __init__(self, bus: FrameBus, writer: SharedFrameWriter,
                 preview: Callable[[Image.Image], None] | None = None) -> None:
        super().__init__(name="onlycam-publisher", daemon=True)
        self._bus = bus
        self._writer = writer
        self._preview = preview
        self._stop_event = threading.Event()
        self.stats = Stats()
        self.default_size: tuple[int, int] = (1280, 720)
        self.default_fps = 30
        self.mirror = False
        self.rotation = 0

    def stop(self) -> None:
        self._stop_event.set()

    def run(self) -> None:
        decoded: Image.Image | None = None
        decoded_counter = -1
        frames_out = 0
        frames_in = 0
        window_start = time.monotonic()
        last_preview = 0.0

        while not self._stop_event.is_set():
            wanted = self._writer.wanted()
            width, height, fps = wanted if wanted else (*self.default_size, self.default_fps)
            fps = max(1, min(fps, 60))
            frame_deadline = time.monotonic() + 1.0 / fps

            jpeg, timestamp, counter = self._bus.latest()
            fresh = jpeg is not None and (time.monotonic() - timestamp) < STALE_AFTER_SECONDS
            if fresh and counter != decoded_counter:
                try:
                    decoded = self._decode(jpeg)
                    decoded_counter = counter
                    frames_in += 1
                except Exception as error:  # noqa: BLE001 - surfaced in the UI
                    self.stats.last_error = f"Кадр не декодирован: {error}"
            if not fresh:
                decoded = None

            if decoded is not None:
                try:
                    frame = ImageOps.pad(decoded, (width, height), color=(0, 0, 0),
                                         method=Image.BILINEAR)
                    array = np.asarray(frame, dtype=np.uint8)
                    # DIB layout expected by DirectShow: BGR, bottom-up.
                    payload = np.ascontiguousarray(array[::-1, :, ::-1]).tobytes()
                    self._writer.write(payload, width, height)
                    frames_out += 1
                    self.stats.size = (width, height)
                except Exception as error:  # noqa: BLE001 - surfaced in the UI
                    self.stats.last_error = f"Кадр не отправлен: {error}"

                now = time.monotonic()
                if self._preview and now - last_preview > 1 / 15:
                    last_preview = now
                    self._preview(decoded)

            now = time.monotonic()
            if now - window_start >= 1.0:
                elapsed = now - window_start
                self.stats.input_fps = frames_in / elapsed
                self.stats.output_fps = frames_out / elapsed
                self.stats.consumers = self._writer.consumers
                frames_in = 0
                frames_out = 0
                window_start = now

            remaining = frame_deadline - time.monotonic()
            if remaining > 0:
                self._stop_event.wait(remaining)

    def _decode(self, jpeg: bytes) -> Image.Image:
        image = Image.open(io.BytesIO(jpeg))
        image = image.convert("RGB")
        if self.rotation:
            image = image.rotate(-self.rotation, expand=True)
        if self.mirror:
            image = ImageOps.mirror(image)
        return image
