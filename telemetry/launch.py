#!/usr/bin/env python3
"""
Ground Station Launcher
--------------------------------------------------------------------------------
Unified TUI that starts the telemetry logger, history API, and Vite dashboard.
MQTT broker (Mosquitto) must be started separately — its status is shown but
the launcher does not manage it.

Run from the laptop/ directory:
    source logger/.venv/bin/activate
    python launch.py

Logs are also written to laptop/logs/<service>.log so you can inspect or
copy them at any time from another terminal:
    tail -f logs/api.log
--------------------------------------------------------------------------------
"""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import signal
import socket
import sys
from datetime import datetime
from pathlib import Path

from rich.markup import escape
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Grid
from textual.reactive import reactive
from textual.widgets import Footer, Header, RichLog, Static

# -- Paths ----------------------------------------------------------------------
HERE       = Path(__file__).parent.resolve()
VENV_BIN   = HERE / "logger" / ".venv" / "bin"
LOGGER_DIR = HERE / "logger"
DASH_DIR   = HERE / "dashboard"
LOGS_DIR   = HERE / "logs"

# -- Strip ANSI codes before writing to Rich markup -----------------------------
_ANSI = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
def _clean(text: str) -> str:
    return _ANSI.sub("", text)

# -- Port helpers ---------------------------------------------------------------
def port_open(host: str = "localhost", port: int = 1883, timeout: float = 0.4) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        return s.connect_ex((host, port)) == 0

async def free_port(port: int) -> bool:
    """SIGTERM any process currently listening on the given TCP port.
    Returns True if something was killed."""
    lsof = shutil.which("lsof")
    if not lsof:
        return False
    proc = await asyncio.create_subprocess_exec(
        lsof, "-ti", f":{port}", "-sTCP:LISTEN",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    stdout, _ = await proc.communicate()
    pids = [p.strip() for p in stdout.decode().split() if p.strip()]
    for pid_str in pids:
        try:
            os.kill(int(pid_str), signal.SIGTERM)
        except (ProcessLookupError, ValueError):
            pass
    if pids:
        await asyncio.sleep(0.6)   # give the process a moment to exit
    return bool(pids)

# -- Status icons ---------------------------------------------------------------
_ICON = {"pending": "○", "running": "●", "stopped": "■", "error": "✗"}
_ICOL = {"pending": "dim white", "running": "bright_green",
         "stopped": "yellow",    "error":   "bright_red"}

# -- Service panel widget -------------------------------------------------------
class ServicePanel(Static):
    """Header + scrolling log for one service."""

    status: reactive[str] = reactive("pending")

    def __init__(self, key: str, label: str, accent: str, **kwargs) -> None:
        super().__init__(**kwargs)
        self._key    = key
        self._label  = label
        self._accent = accent

    def compose(self) -> ComposeResult:
        yield Static("", id=f"hdr-{self._key}", classes="svc-hdr")
        yield RichLog(id=f"log-{self._key}", markup=True,
                      highlight=False, wrap=True, classes="svc-log")

    def on_mount(self) -> None:
        self._paint_header()

    def watch_status(self, _: str) -> None:
        self._paint_header()

    def _paint_header(self) -> None:
        icon = _ICON.get(self.status, "○")
        icol = _ICOL.get(self.status, "dim white")
        log_hint = f"[dim]  logs/{self._key}.log[/dim]"
        self.query_one(f"#hdr-{self._key}", Static).update(
            f" [{icol}]{icon}[/{icol}]  [{self._accent}]{self._label}[/{self._accent}]{log_hint}"
        )

    def writeln(self, line: str, *, err: bool = False) -> None:
        ts   = datetime.now().strftime("%H:%M:%S")
        col  = "red" if err else self._accent
        safe = escape(_clean(line))
        self.query_one(RichLog).write(f"[dim]{ts}[/dim]  [{col}]{safe}[/{col}]")

    def info(self, msg: str) -> None:
        self.query_one(RichLog).write(f"[dim italic]  {msg}[/dim italic]")


# -- App ------------------------------------------------------------------------
class Launcher(App[None]):
    TITLE = "ROCKETRY AT VT GROUND STATION"
    CSS = """
    Launcher { background: #040408; }
    Header   { background: #0d0d1a; color: #00ff88; text-style: bold; }
    Footer   { background: #0d0d1a; color: #444466; }

    Grid {
        grid-size: 2 2;
        grid-gutter: 1;
        padding: 1;
        height: 1fr;
    }

    ServicePanel {
        border: solid #1a1a2e;
        height: 1fr;
    }

    .svc-hdr {
        background: #0d0d1a;
        height: 1;
        padding: 0 1;
    }

    .svc-log {
        background: #040408;
        padding: 0 1;
        height: 1fr;
        scrollbar-size: 1 1;
        scrollbar-background: #0d0d1a;
        scrollbar-color: #222240;
    }
    """

    BINDINGS = [
        Binding("q", "quit_all",     "Quit",           show=True),
        Binding("r", "restart_all",  "Restart",        show=True),
        Binding("o", "open_browser", "Open dashboard", show=True),
        Binding("l", "open_logs",    "Open logs",      show=True),
    ]

    # Grid order: top-left, top-right, bottom-left, bottom-right
    _SERVICES = [
        dict(key="broker",    label="MQTT BROKER",      accent="#ff8844",
             monitor_only=True),
        dict(key="logger",    label="TELEMETRY LOGGER", accent="#00ff88",
             cmd=[str(VENV_BIN / "python"), "logger.py"],
             cwd=LOGGER_DIR),
        dict(key="api",       label="HISTORY API",      accent="#4488ff",
             cmd=[str(VENV_BIN / "uvicorn"), "api:app",
                  "--port", "8000", "--log-level", "warning"],
             cwd=LOGGER_DIR,
             port=8000),                                  # freed before start
        dict(key="dashboard", label="DASHBOARD",        accent="#aa44ff",
             cmd=["npm", "run", "dev"],
             cwd=DASH_DIR),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._procs: dict[str, asyncio.subprocess.Process] = {}
        self._tasks: list[asyncio.Task] = []

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Grid():
            for svc in self._SERVICES:
                yield ServicePanel(svc["key"], svc["label"], svc["accent"],
                                   id=f"panel-{svc['key']}")
        yield Footer()

    def on_mount(self) -> None:
        LOGS_DIR.mkdir(exist_ok=True)
        self._tasks.append(asyncio.create_task(self._start_all()))

    # -- Startup ----------------------------------------------------------------
    async def _start_all(self) -> None:
        await asyncio.gather(
            self._watch_broker(),
            *(self._run_service(s) for s in self._SERVICES if not s.get("monitor_only")),
        )

    async def _watch_broker(self) -> None:
        panel: ServicePanel = self.query_one("#panel-broker", ServicePanel)
        panel.info("checking localhost:1883 / :9001 …")
        while True:
            mqtt_up = port_open("localhost", 1883)
            ws_up   = port_open("localhost", 9001)
            if mqtt_up and ws_up:
                if panel.status != "running":
                    panel.status = "running"
                    panel.info("broker up  :1883 (MQTT)  :9001 (WebSocket)")
            elif mqtt_up:
                if panel.status != "stopped":
                    panel.status = "stopped"
                    panel.info(":1883 up but :9001 missing — dashboard won't receive live data")
            else:
                if panel.status not in ("pending", "error"):
                    panel.status = "error"
                    panel.info("broker offline — start Mosquitto separately")
                elif panel.status == "pending":
                    panel.info("waiting for broker on :1883 …")
            await asyncio.sleep(3)

    async def _run_service(self, svc: dict) -> None:
        key   = svc["key"]
        panel: ServicePanel = self.query_one(f"#panel-{key}", ServicePanel)
        cmd   = svc["cmd"]

        if not Path(cmd[0]).exists() and not shutil.which(cmd[0]):
            panel.status = "error"
            panel.info(f"not found: {cmd[0]}")
            return

        # Free the port if another process is already using it.
        if port := svc.get("port"):
            if port_open("localhost", port):
                panel.info(f"port {port} in use — stopping existing process …")
                killed = await free_port(port)
                if killed:
                    panel.info(f"cleared port {port}")

        panel.info(f"$ {' '.join(cmd)}")

        log_path = LOGS_DIR / f"{key}.log"
        proc: asyncio.subprocess.Process | None = None
        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                cwd=svc.get("cwd"),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                env={**os.environ, "NO_COLOR": "1", "CI": "true"},
            )
            self._procs[key] = proc
            panel.status = "running"

            with open(log_path, "w") as log_file:
                log_file.write(f"=== {key} started {datetime.now():%Y-%m-%d %H:%M:%S} ===\n")
                log_file.flush()

                assert proc.stdout
                while True:
                    raw = await proc.stdout.readline()
                    if not raw:
                        break
                    line = raw.decode(errors="replace").rstrip()
                    if line:
                        panel.writeln(line)
                        log_file.write(f"{datetime.now():%H:%M:%S}  {_clean(line)}\n")
                        log_file.flush()

            rc = proc.returncode
            panel.status = "stopped" if rc in (0, -15, None) else "error"
            panel.info(f"exited (code {rc})")

        except asyncio.CancelledError:
            pass
        except FileNotFoundError as exc:
            panel.status = "error"
            panel.info(f"command not found: {exc}")
        except Exception as exc:
            panel.status = "error"
            panel.info(f"error: {exc}")
        finally:
            if proc and proc.returncode is None:
                try:
                    proc.terminate()
                    await asyncio.wait_for(proc.wait(), timeout=3.0)
                except Exception:
                    proc.kill()

    # -- Actions ----------------------------------------------------------------
    async def action_quit_all(self) -> None:
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self.exit()

    async def action_restart_all(self) -> None:
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._procs.clear()
        self._tasks.clear()

        for svc in self._SERVICES:
            panel = self.query_one(f"#panel-{svc['key']}", ServicePanel)
            panel.status = "pending"
            panel.query_one(RichLog).clear()

        self._tasks.append(asyncio.create_task(self._start_all()))

    async def action_open_browser(self) -> None:
        opener = shutil.which("open") or shutil.which("xdg-open")
        if opener:
            await asyncio.create_subprocess_exec(opener, "http://localhost:3000")

    async def action_open_logs(self) -> None:
        LOGS_DIR.mkdir(exist_ok=True)
        opener = shutil.which("open") or shutil.which("xdg-open")
        if opener:
            await asyncio.create_subprocess_exec(opener, str(LOGS_DIR))


if __name__ == "__main__":
    if not (VENV_BIN / "python").exists():
        print(
            "ERROR: logger/.venv not found.\n"
            "Run: cd logger && python3 -m venv .venv && "
            ".venv/bin/pip install -r requirements.txt",
            file=sys.stderr,
        )
        sys.exit(1)

    Launcher().run()
