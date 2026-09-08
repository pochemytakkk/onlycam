"""Installation state of the OnlyCam DirectShow filter."""

from __future__ import annotations

import ctypes
import sys
from pathlib import Path

CLSID = "{87ABDBEA-15FF-4540-8574-06F0888F609A}"


def app_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


def bundled_filters() -> list[Path]:
    directory = app_dir() / "filter"
    return [path for path in (directory / "OnlyCamFilter64.dll", directory / "OnlyCamFilter32.dll")
            if path.exists()]


def registered_path() -> str | None:
    if sys.platform != "win32":
        return None
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, f"CLSID\\{CLSID}\\InprocServer32") as key:
            value, _ = winreg.QueryValueEx(key, None)
            return value
    except OSError:
        return None


def is_installed() -> bool:
    return registered_path() is not None


def _run_regsvr32(paths: list[Path] | None, unregister_flag: bool) -> bool:
    """Runs regsvr32 for every DLL behind a single elevation prompt."""
    if sys.platform != "win32":
        return False
    targets = paths if paths is not None else bundled_filters()
    if not targets:
        return False
    flags = "/s /u" if unregister_flag else "/s"
    commands = " && ".join(f'regsvr32.exe {flags} "{path}"' for path in targets)
    result = ctypes.windll.shell32.ShellExecuteW(
        None, "runas", "cmd.exe", f'/c {commands}', None, 0
    )
    return int(result) > 32


def register(paths: list[Path] | None = None) -> bool:
    """Registers the bundled filter DLLs, prompting for elevation."""
    return _run_regsvr32(paths, unregister_flag=False)


def unregister(paths: list[Path] | None = None) -> bool:
    return _run_regsvr32(paths, unregister_flag=True)
