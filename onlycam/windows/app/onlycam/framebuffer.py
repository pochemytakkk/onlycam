"""Writer side of the shared memory contract with the OnlyCam DirectShow filter.

Layout mirrors windows/filter/src/framebuffer.h.
"""

from __future__ import annotations

import ctypes
import struct
import sys
import time

MAPPING_NAME = "Local\\OnlyCamFrameBuffer"
MAGIC = 0x3143464F  # "OCF1"
VERSION = 1
HEADER_SIZE = 256
MAX_WIDTH = 1920
MAX_HEIGHT = 1080
MAX_FRAME_BYTES = MAX_WIDTH * MAX_HEIGHT * 3
MAPPING_SIZE = HEADER_SIZE + MAX_FRAME_BYTES
FORMAT_BGR24 = 0

OFF_MAGIC = 0
OFF_VERSION = 4
OFF_HEADER_SIZE = 8
OFF_MAX_WIDTH = 12
OFF_MAX_HEIGHT = 16
OFF_SEQUENCE = 20
OFF_WIDTH = 24
OFF_HEIGHT = 28
OFF_FORMAT = 32
OFF_FRAME_INDEX = 36
OFF_SENDER_TICK = 44
OFF_SENDER_ACTIVE = 52
OFF_CONSUMER_COUNT = 56
OFF_WANT_WIDTH = 60
OFF_WANT_HEIGHT = 64
OFF_WANT_FPS = 68
OFF_CONSUMER_TICK = 72

# Everyone may open the section; low integrity sandboxes (Chromium, Discord)
# may read from it.
_SDDL = "D:(A;;GA;;;WD)S:(ML;;NW;;;LW)"
_PAGE_READWRITE = 0x04
_FILE_MAP_ALL_ACCESS = 0xF001F
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1)


def tick_ms() -> int:
    """Same time base as GetTickCount64 used by the filter."""
    if sys.platform == "win32":
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.GetTickCount64.restype = ctypes.c_ulonglong
        return int(kernel32.GetTickCount64())
    return int(time.monotonic() * 1000)


class _SecurityAttributes(ctypes.Structure):
    _fields_ = [
        ("nLength", ctypes.c_ulong),
        ("lpSecurityDescriptor", ctypes.c_void_p),
        ("bInheritHandle", ctypes.c_int),
    ]


class SharedFrameWriter:
    """Publishes BGR24 bottom-up frames into the section read by the filter."""

    def __init__(self) -> None:
        self._buffer: ctypes.Array | None = None
        self._view: int | None = None
        self._handle: int | None = None
        self._frame_index = 0

    # -- lifecycle ---------------------------------------------------------
    def open(self) -> None:
        if self._buffer is not None:
            return
        if sys.platform == "win32":
            self._open_windows()
        else:  # Development fallback so the pipeline can be exercised on Linux.
            self._buffer = (ctypes.c_char * MAPPING_SIZE)()
        self._init_header()

    def _open_windows(self) -> None:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

        descriptor = ctypes.c_void_p()
        attributes_ptr = None
        attributes = _SecurityAttributes()
        if advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            ctypes.c_wchar_p(_SDDL), 1, ctypes.byref(descriptor), None
        ):
            attributes.nLength = ctypes.sizeof(_SecurityAttributes)
            attributes.lpSecurityDescriptor = descriptor
            attributes.bInheritHandle = 0
            attributes_ptr = ctypes.byref(attributes)

        kernel32.CreateFileMappingW.restype = ctypes.c_void_p
        handle = kernel32.CreateFileMappingW(
            _INVALID_HANDLE_VALUE, attributes_ptr, _PAGE_READWRITE, 0, MAPPING_SIZE,
            ctypes.c_wchar_p(MAPPING_NAME),
        )
        if descriptor:
            kernel32.LocalFree(descriptor)
        if not handle:
            raise OSError(ctypes.get_last_error(), "CreateFileMappingW failed")

        kernel32.MapViewOfFile.restype = ctypes.c_void_p
        kernel32.MapViewOfFile.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_size_t,
        ]
        view = kernel32.MapViewOfFile(handle, _FILE_MAP_ALL_ACCESS, 0, 0, MAPPING_SIZE)
        if not view:
            error = ctypes.get_last_error()
            kernel32.CloseHandle(ctypes.c_void_p(handle))
            raise OSError(error, "MapViewOfFile failed")

        self._handle = handle
        self._view = view
        self._buffer = (ctypes.c_char * MAPPING_SIZE).from_address(view)

    def _init_header(self) -> None:
        if self._read_u32(OFF_MAGIC) != MAGIC:
            ctypes.memset(self._buffer, 0, HEADER_SIZE)
            self._write_u32(OFF_MAGIC, MAGIC)
            self._write_u32(OFF_VERSION, VERSION)
            self._write_u32(OFF_HEADER_SIZE, HEADER_SIZE)
            self._write_u32(OFF_MAX_WIDTH, MAX_WIDTH)
            self._write_u32(OFF_MAX_HEIGHT, MAX_HEIGHT)
        self._write_u32(OFF_FORMAT, FORMAT_BGR24)

    def close(self) -> None:
        if self._buffer is None:
            return
        try:
            self._write_u32(OFF_SENDER_ACTIVE, 0)
        except Exception:
            pass
        if sys.platform == "win32" and self._view:
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.UnmapViewOfFile(ctypes.c_void_p(self._view))
            kernel32.CloseHandle(ctypes.c_void_p(self._handle))
        self._buffer = None
        self._view = None
        self._handle = None

    # -- header access -----------------------------------------------------
    def _read_u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self._buffer, offset)[0]

    def _write_u32(self, offset: int, value: int) -> None:
        struct.pack_into("<I", self._buffer, offset, value)

    def _write_u64(self, offset: int, value: int) -> None:
        struct.pack_into("<Q", self._buffer, offset, value)

    @property
    def consumers(self) -> int:
        return self._read_u32(OFF_CONSUMER_COUNT) if self._buffer is not None else 0

    def wanted(self) -> tuple[int, int, int] | None:
        """Resolution and fps requested by the connected application."""
        if self._buffer is None:
            return None
        width = self._read_u32(OFF_WANT_WIDTH)
        height = self._read_u32(OFF_WANT_HEIGHT)
        fps = self._read_u32(OFF_WANT_FPS) or 30
        if width == 0 or height == 0:
            return None
        return width, height, fps

    # -- frame publishing --------------------------------------------------
    def write(self, frame: bytes, width: int, height: int) -> None:
        if self._buffer is None:
            raise RuntimeError("frame buffer is not open")
        expected = width * height * 3
        if width > MAX_WIDTH or height > MAX_HEIGHT or len(frame) != expected:
            raise ValueError(f"unexpected frame {width}x{height} of {len(frame)} bytes")

        sequence = self._read_u32(OFF_SEQUENCE)
        self._write_u32(OFF_SEQUENCE, sequence + 1)  # odd: write in progress
        ctypes.memmove(ctypes.byref(self._buffer, HEADER_SIZE), frame, expected)
        self._write_u32(OFF_WIDTH, width)
        self._write_u32(OFF_HEIGHT, height)
        self._frame_index += 1
        self._write_u64(OFF_FRAME_INDEX, self._frame_index)
        self._write_u32(OFF_SEQUENCE, sequence + 2)  # even: frame is readable

        self._write_u64(OFF_SENDER_TICK, tick_ms())
        self._write_u32(OFF_SENDER_ACTIVE, 1)

    def heartbeat(self) -> None:
        if self._buffer is not None:
            self._write_u64(OFF_SENDER_TICK, tick_ms())
            self._write_u32(OFF_SENDER_ACTIVE, 1)
