#!/usr/bin/env python3
"""Run the ground-station USB altitude bridge from the Tauri app directory."""

from __future__ import annotations

import runpy
from pathlib import Path


if __name__ == "__main__":
    script = Path(__file__).resolve().parents[3] / "tools" / "usb_altitude_bridge.py"
    runpy.run_path(str(script), run_name="__main__")
