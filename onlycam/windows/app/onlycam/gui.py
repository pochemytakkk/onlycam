"""Tkinter desktop UI for OnlyCam."""

from __future__ import annotations

import queue
import tkinter as tk
from tkinter import messagebox, ttk

import qrcode
from PIL import Image, ImageTk

from . import net, vcam
from .framebuffer import SharedFrameWriter
from .pipeline import Publisher
from .sources import BrowserSource, MjpegPullSource, Source

PREVIEW_SIZE = (480, 270)
DEFAULT_PORT = 8443
DEFAULT_IOS_URL = "http://192.168.0.10:8080/stream"

SOURCE_BROWSER = "Телефон через браузер (QR-код)"
SOURCE_IOS = "Приложение OnlyCam на iPhone"


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("OnlyCam")
        self.geometry("980x560")
        self.minsize(900, 520)

        self._writer = SharedFrameWriter()
        self._bus_source: Source | None = None
        self._publisher: Publisher | None = None
        self._preview_queue: queue.Queue[Image.Image] = queue.Queue(maxsize=1)
        self._preview_image: ImageTk.PhotoImage | None = None
        self._qr_image: ImageTk.PhotoImage | None = None
        self._running = False

        self._build_ui()
        self._refresh_filter_state()
        self.after(100, self._tick)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # -- layout ------------------------------------------------------------
    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1)
        root.columnconfigure(1, weight=0)
        root.rowconfigure(0, weight=1)

        left = ttk.Frame(root)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        self._preview = tk.Label(left, background="#101216", text="Нет сигнала",
                                 foreground="#8a8f98")
        self._preview.pack(fill="both", expand=True)
        self._stats = ttk.Label(left, text="")
        self._stats.pack(anchor="w", pady=(8, 0))

        right = ttk.Frame(root, width=330)
        right.grid(row=0, column=1, sticky="ns")

        ttk.Label(right, text="Источник видео").pack(anchor="w")
        self._source_var = tk.StringVar(value=SOURCE_BROWSER)
        source_box = ttk.Combobox(right, textvariable=self._source_var, state="readonly",
                                  values=[SOURCE_BROWSER, SOURCE_IOS], width=36)
        source_box.pack(fill="x", pady=(2, 10))
        source_box.bind("<<ComboboxSelected>>", lambda _event: self._on_source_change())

        options = ttk.Frame(right)
        options.pack(fill="x")

        self._ios_frame = ttk.Frame(options)
        ttk.Label(self._ios_frame, text="Адрес потока с iPhone").pack(anchor="w")
        self._ios_url = tk.StringVar(value=DEFAULT_IOS_URL)
        ttk.Entry(self._ios_frame, textvariable=self._ios_url).pack(fill="x", pady=(2, 10))

        self._qr_frame = ttk.Frame(options)
        self._qr_label = ttk.Label(self._qr_frame)
        self._qr_label.pack()
        self._qr_text = ttk.Label(self._qr_frame, text="", wraplength=300, justify="center")
        self._qr_text.pack(pady=(6, 10))

        settings = ttk.Frame(right)
        settings.pack(fill="x", pady=(0, 10))
        ttk.Label(settings, text="Разрешение").grid(row=0, column=0, sticky="w")
        self._resolution = tk.StringVar(value="1280x720")
        ttk.Combobox(settings, textvariable=self._resolution, state="readonly", width=12,
                     values=["1920x1080", "1280x720", "640x480"]).grid(row=0, column=1, sticky="e")
        ttk.Label(settings, text="Кадры/с").grid(row=1, column=0, sticky="w", pady=(6, 0))
        self._fps = tk.StringVar(value="30")
        ttk.Combobox(settings, textvariable=self._fps, state="readonly", width=12,
                     values=["30", "24", "15"]).grid(row=1, column=1, sticky="e", pady=(6, 0))
        settings.columnconfigure(0, weight=1)

        self._mirror = tk.BooleanVar(value=False)
        ttk.Checkbutton(right, text="Зеркалить", variable=self._mirror,
                        command=self._apply_transform).pack(anchor="w")
        ttk.Label(right, text="Поворот").pack(anchor="w", pady=(8, 0))
        self._rotation = tk.StringVar(value="0°")
        ttk.Combobox(right, textvariable=self._rotation, state="readonly", width=12,
                     values=["0°", "90°", "180°", "270°"]).pack(anchor="w")
        self._rotation.trace_add("write", lambda *_: self._apply_transform())

        self._toggle = ttk.Button(right, text="Включить камеру", command=self._toggle_stream)
        self._toggle.pack(fill="x", pady=(14, 6))

        self._filter_label = ttk.Label(right, text="", wraplength=300, foreground="#a33")
        self._filter_label.pack(anchor="w")
        self._install_button = ttk.Button(right, text="Установить виртуальную камеру",
                                          command=self._install_filter)
        self._install_button.pack(fill="x", pady=(6, 0))

        self._status = ttk.Label(right, text="", wraplength=300)
        self._status.pack(anchor="w", pady=(12, 0))

        self._on_source_change()

    # -- state -------------------------------------------------------------
    def _on_source_change(self) -> None:
        if self._source_var.get() == SOURCE_IOS:
            self._qr_frame.pack_forget()
            self._ios_frame.pack(fill="x")
        else:
            self._ios_frame.pack_forget()
            self._qr_frame.pack(fill="x")
            self._render_qr(f"https://{net.primary_ip()}:{DEFAULT_PORT}/")

    def _render_qr(self, url: str) -> None:
        code = qrcode.QRCode(border=2, box_size=5)
        code.add_data(url)
        code.make(fit=True)
        image = code.make_image(fill_color="black", back_color="white").convert("RGB")
        self._qr_image = ImageTk.PhotoImage(image)
        self._qr_label.configure(image=self._qr_image)
        self._qr_text.configure(text=f"Открой на телефоне:\n{url}\n"
                                     "Safari покажет предупреждение о сертификате — "
                                     "«Подробнее» → «Посетить этот веб-сайт».")

    def _refresh_filter_state(self) -> None:
        installed = vcam.is_installed()
        if installed:
            self._filter_label.configure(text="Виртуальная камера «OnlyCam» установлена.",
                                         foreground="#2a7")
            self._install_button.configure(text="Переустановить виртуальную камеру")
        else:
            self._filter_label.configure(
                text="Виртуальная камера не зарегистрирована — "
                     "OnlyCam не появится в OBS и Discord.",
                foreground="#a33")
            self._install_button.configure(text="Установить виртуальную камеру")

    def _install_filter(self) -> None:
        if not vcam.bundled_filters():
            messagebox.showerror("OnlyCam", "Не найдены файлы фильтра рядом с приложением "
                                            "(папка filter).")
            return
        if vcam.register():
            self.after(1500, self._refresh_filter_state)
        else:
            messagebox.showerror("OnlyCam", "Регистрация отменена или не удалась.")

    def _apply_transform(self) -> None:
        if self._publisher:
            self._publisher.mirror = self._mirror.get()
            self._publisher.rotation = int(self._rotation.get().rstrip("°"))

    # -- streaming ---------------------------------------------------------
    def _toggle_stream(self) -> None:
        if self._running:
            self._stop_stream()
        else:
            self._start_stream()

    def _start_stream(self) -> None:
        from .sources import FrameBus

        try:
            self._writer.open()
        except Exception as error:  # noqa: BLE001 - surfaced in the UI
            messagebox.showerror("OnlyCam", f"Не удалось открыть общую память: {error}")
            return

        bus = FrameBus()
        if self._source_var.get() == SOURCE_IOS:
            source: Source = MjpegPullSource(bus, self._ios_url.get().strip())
        else:
            source = BrowserSource(bus, port=DEFAULT_PORT)

        try:
            source.start()
        except Exception as error:  # noqa: BLE001 - surfaced in the UI
            messagebox.showerror("OnlyCam", f"Источник не запустился: {error}")
            return

        width, height = (int(value) for value in self._resolution.get().split("x"))
        publisher = Publisher(bus, self._writer, preview=self._queue_preview)
        publisher.default_size = (width, height)
        publisher.default_fps = int(self._fps.get())
        publisher.mirror = self._mirror.get()
        publisher.rotation = int(self._rotation.get().rstrip("°"))
        publisher.start()

        self._bus_source = source
        self._publisher = publisher
        self._running = True
        self._toggle.configure(text="Выключить камеру")
        if isinstance(source, BrowserSource):
            self._render_qr(source.url)

    def _stop_stream(self) -> None:
        if self._publisher:
            self._publisher.stop()
            self._publisher.join(timeout=3)
            self._publisher = None
        if self._bus_source:
            self._bus_source.stop()
            self._bus_source = None
        self._writer.close()
        self._running = False
        self._toggle.configure(text="Включить камеру")
        self._preview.configure(image="", text="Нет сигнала")
        self._preview_image = None

    def _queue_preview(self, image: Image.Image) -> None:
        try:
            self._preview_queue.put_nowait(image)
        except queue.Full:
            pass

    # -- periodic ----------------------------------------------------------
    def _tick(self) -> None:
        try:
            image = self._preview_queue.get_nowait()
        except queue.Empty:
            image = None
        if image is not None:
            preview = image.copy()
            preview.thumbnail(PREVIEW_SIZE)
            self._preview_image = ImageTk.PhotoImage(preview)
            self._preview.configure(image=self._preview_image, text="")

        if self._publisher:
            stats = self._publisher.stats
            self._stats.configure(
                text=f"Вход {stats.input_fps:.0f} к/с · выход {stats.output_fps:.0f} к/с · "
                     f"{stats.size[0]}x{stats.size[1]} · программ подключено: {stats.consumers}"
                     + (f" · {stats.last_error}" if stats.last_error else "")
            )
        if self._bus_source:
            self._status.configure(text=self._bus_source.status)

        self.after(60, self._tick)

    def _on_close(self) -> None:
        self._stop_stream()
        self.destroy()


def main() -> int:
    app = App()
    app.mainloop()
    return 0
