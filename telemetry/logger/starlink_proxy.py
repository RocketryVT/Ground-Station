"""
Starlink dish proxy — polls 192.168.100.1:9200 via gRPC and serves the
decoded data as JSON on http://localhost:8001/starlink.

The generated protobuf stubs are compiled from spacex/api/device/dish.proto
on first run if they don't exist yet.  No pre-compilation step needed.

Run standalone:
    python starlink_proxy.py

Or let launch.py start it automatically.
"""

from __future__ import annotations

import importlib
import json
import logging
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

log = logging.getLogger("starlink_proxy")
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s  %(levelname)s  %(message)s")

HERE       = Path(__file__).parent
PROTO_DIR  = HERE / "spacex" / "api" / "device"
PROTO_FILE = PROTO_DIR / "dish.proto"
STUB_FILE  = PROTO_DIR / "dish_pb2.py"

DISH_HOST  = "192.168.100.1"
DISH_PORT  = 9200
POLL_EVERY = 2.0   # seconds
PROXY_PORT = 8001


# -- Compile stubs on demand ---------------------------------------------------

def _ensure_stubs() -> None:
    if STUB_FILE.exists():
        return
    log.info("compiling protobuf stubs from %s", PROTO_FILE)
    result = subprocess.run(
        [
            sys.executable, "-m", "grpc_tools.protoc",
            f"--proto_path={HERE}",
            f"--python_out={HERE}",
            f"--grpc_python_out={HERE}",
            str(PROTO_FILE.relative_to(HERE)),
        ],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"protoc failed:\n{result.stderr}")
    log.info("stubs compiled successfully")


def _load_stubs():
    _ensure_stubs()
    # Add HERE to sys.path so the generated imports resolve
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    pb2      = importlib.import_module("spacex.api.device.dish_pb2")
    pb2_grpc = importlib.import_module("spacex.api.device.dish_pb2_grpc")
    return pb2, pb2_grpc


# -- Poller --------------------------------------------------------------------

_state: dict = {"data": {}, "error": None, "last_ok": None}
_lock  = threading.Lock()


def _poll_loop(pb2, pb2_grpc) -> None:
    import grpc

    channel = grpc.insecure_channel(f"{DISH_HOST}:{DISH_PORT}")
    stub    = pb2_grpc.DeviceStub(channel)

    while True:
        try:
            loc_resp  = stub.Handle(pb2.Request(get_location=pb2.GetLocationRequest()))
            stat_resp = stub.Handle(pb2.Request(get_status=pb2.GetStatusRequest()))

            loc  = loc_resp.get_location
            stat = stat_resp.dish_get_status

            data = {
                # location
                "lat":                     loc.lla.lat,
                "lon":                     loc.lla.lon,
                "alt":                     loc.lla.alt,
                "horizontal_speed_mps":    loc.horizontal_speed_mps,
                "vertical_speed_mps":      loc.vertical_speed_mps,
                # connectivity
                "pop_ping_latency_ms":     stat.pop_ping_latency_ms,
                "pop_ping_drop_rate":      stat.pop_ping_drop_rate,
                "downlink_throughput_bps": stat.downlink_throughput_bps,
                "uplink_throughput_bps":   stat.uplink_throughput_bps,
                # pointing
                "boresight_azimuth_deg":   stat.boresight_azimuth_deg,
                "boresight_elevation_deg": stat.boresight_elevation_deg,
                # GPS
                "gps_valid":               stat.gps_stats.gps_valid,
                "gps_sats":                stat.gps_stats.gps_sats,
                # device
                "hardware_version":        stat.device_info.hardware_version,
                "software_version":        stat.device_info.software_version,
                "uptime_s":                stat.device_state.uptime_s,
            }

            with _lock:
                _state["data"]    = data
                _state["error"]   = None
                _state["last_ok"] = time.time()

            log.info("poll ok — lat=%.4f lon=%.4f latency=%.1f ms",
                     loc.lla.lat, loc.lla.lon, stat.pop_ping_latency_ms)

        except Exception as exc:
            import grpc
            # Extract just the human-readable details from gRPC errors;
            # the full RpcError string is extremely verbose.
            if isinstance(exc, grpc.RpcError):
                short = exc.details()   # e.g. "failed to connect to all addresses"
            else:
                short = str(exc)

            with _lock:
                _state["error"] = short

            # Only log once when transitioning to error, not every poll cycle
            if _state.get("_was_ok", True):
                log.warning("dish unreachable: %s", short)
                _state["_was_ok"] = False

            time.sleep(15)   # back off — no point hammering an unreachable host
            continue

        _state["_was_ok"] = True
        time.sleep(POLL_EVERY)


# -- HTTP server ---------------------------------------------------------------

class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass   # silence default access log

    def do_GET(self):
        if self.path not in ("/starlink", "/starlink/"):
            self.send_response(404)
            self.end_headers()
            return

        with _lock:
            payload = json.dumps({
                **_state["data"],
                "error":   _state["error"],
                "last_ok": _state["last_ok"],
            })

        body = payload.encode()
        self.send_response(200)
        self.send_header("Content-Type",                "application/json")
        self.send_header("Content-Length",              str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.end_headers()


if __name__ == "__main__":
    pb2, pb2_grpc = _load_stubs()

    t = threading.Thread(target=_poll_loop, args=(pb2, pb2_grpc), daemon=True)
    t.start()

    server = HTTPServer(("", PROXY_PORT), _Handler)
    log.info("Starlink proxy listening on http://localhost:%d/starlink", PROXY_PORT)
    log.info("Polling dish at %s:%d every %.0f s", DISH_HOST, DISH_PORT, POLL_EVERY)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
