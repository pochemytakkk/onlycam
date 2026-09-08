import io
import struct
import time

import numpy as np
from PIL import Image

from onlycam import framebuffer as fb
from onlycam.pipeline import Publisher
from onlycam.sources import FrameBus


def _jpeg(width: int, height: int, color=(200, 30, 40)) -> bytes:
    buffer = io.BytesIO()
    Image.new("RGB", (width, height), color).save(buffer, format="JPEG", quality=95)
    return buffer.getvalue()


def _read_frame(writer: fb.SharedFrameWriter):
    raw = bytes(writer._buffer)  # noqa: SLF001 - reader side lives in the C++ filter
    width = struct.unpack_from("<I", raw, fb.OFF_WIDTH)[0]
    height = struct.unpack_from("<I", raw, fb.OFF_HEIGHT)[0]
    sequence = struct.unpack_from("<I", raw, fb.OFF_SEQUENCE)[0]
    payload = raw[fb.HEADER_SIZE:fb.HEADER_SIZE + width * height * 3]
    return width, height, sequence, payload


def test_header_is_initialised():
    writer = fb.SharedFrameWriter()
    writer.open()
    try:
        raw = bytes(writer._buffer)  # noqa: SLF001
        assert struct.unpack_from("<I", raw, fb.OFF_MAGIC)[0] == fb.MAGIC
        assert struct.unpack_from("<I", raw, fb.OFF_MAX_WIDTH)[0] == fb.MAX_WIDTH
    finally:
        writer.close()


def test_write_marks_frame_readable():
    writer = fb.SharedFrameWriter()
    writer.open()
    try:
        payload = bytes([7]) * (64 * 48 * 3)
        writer.write(payload, 64, 48)
        width, height, sequence, data = _read_frame(writer)
        assert (width, height) == (64, 48)
        assert sequence % 2 == 0  # even: no write in progress
        assert data == payload
    finally:
        writer.close()


def test_publisher_letterboxes_and_publishes_bgr_bottom_up():
    writer = fb.SharedFrameWriter()
    writer.open()
    bus = FrameBus()
    publisher = Publisher(bus, writer)
    publisher.default_size = (320, 240)
    publisher.default_fps = 30
    try:
        bus.publish(_jpeg(160, 120, color=(255, 0, 0)))
        publisher.start()
        deadline = time.monotonic() + 5
        width = 0
        while time.monotonic() < deadline:
            width, height, _, payload = _read_frame(writer)
            if width:
                break
            time.sleep(0.05)
        assert (width, height) == (320, 240)

        frame = np.frombuffer(payload, dtype=np.uint8).reshape(height, width, 3)
        centre = frame[height // 2, width // 2]
        # Red source arrives as BGR.
        assert centre[0] < 40 and centre[2] > 200
        # 4:3 source in a 4:3 target leaves no letterbox bars.
        assert frame[height // 2, 2].tolist() != [0, 0, 0]
    finally:
        publisher.stop()
        publisher.join(timeout=3)
        writer.close()


def test_publisher_follows_requested_size():
    writer = fb.SharedFrameWriter()
    writer.open()
    struct.pack_into("<I", writer._buffer, fb.OFF_WANT_WIDTH, 640)  # noqa: SLF001
    struct.pack_into("<I", writer._buffer, fb.OFF_WANT_HEIGHT, 360)  # noqa: SLF001
    struct.pack_into("<I", writer._buffer, fb.OFF_WANT_FPS, 30)  # noqa: SLF001
    bus = FrameBus()
    publisher = Publisher(bus, writer)
    publisher.default_size = (1280, 720)
    try:
        bus.publish(_jpeg(320, 240))
        publisher.start()
        deadline = time.monotonic() + 5
        size = (0, 0)
        while time.monotonic() < deadline:
            width, height, _, _ = _read_frame(writer)
            if width:
                size = (width, height)
                break
            time.sleep(0.05)
        assert size == (640, 360)
    finally:
        publisher.stop()
        publisher.join(timeout=3)
        writer.close()


def test_stale_frames_are_not_republished():
    writer = fb.SharedFrameWriter()
    writer.open()
    bus = FrameBus()
    publisher = Publisher(bus, writer)
    try:
        bus.publish(_jpeg(64, 48))
        publisher.start()
        time.sleep(0.5)
        _, _, first_sequence, _ = _read_frame(writer)
        assert first_sequence > 0
        bus._timestamp = time.monotonic() - 30  # noqa: SLF001 - simulate a dead phone
        time.sleep(0.5)
        _, _, second_sequence, _ = _read_frame(writer)
        time.sleep(0.3)
        _, _, third_sequence, _ = _read_frame(writer)
        assert second_sequence == third_sequence
    finally:
        publisher.stop()
        publisher.join(timeout=3)
        writer.close()
