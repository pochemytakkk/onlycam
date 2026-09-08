"""Executable entry point used by the PyInstaller build."""

from __future__ import annotations

import logging
import sys

from onlycam.gui import main

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    sys.exit(main())
