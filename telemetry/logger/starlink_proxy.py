"""
Starlink dish proxy — polls 192.168.100.1:9200 via gRPC and serves the
decoded data as protobuf on http://localhost:8001/starlink.

The generated protobuf stubs are compiled from the Starlink spacex_api proto
tree on first run if they don't exist yet.  No pre-compilation step needed.

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
PROTO_DIR  = HERE / "proto"
STUB_FILE  = HERE / "spacex_api" / "device" / "device_pb2.py"

if str(PROTO_DIR) not in sys.path:
    sys.path.insert(0, str(PROTO_DIR))
import ground_station_pb2  # noqa: E402

DISH_HOST  = "192.168.100.1"
DISH_PORT  = 9200
POLL_EVERY = 2.0   # seconds
PROXY_PORT = 8001


# -- Compile stubs on demand ---------------------------------------------------

def _ensure_stubs() -> None:
    if STUB_FILE.exists():
        return

    proto_files = sorted(
        path.relative_to(PROTO_DIR)
        for path in (PROTO_DIR / "spacex_api").rglob("*.proto")
    )

    log.info("compiling protobuf stubs from %s", PROTO_DIR / "spacex_api")
    result = subprocess.run(
        [
            sys.executable, "-m", "grpc_tools.protoc",
            f"--proto_path={PROTO_DIR}",
            f"--python_out={HERE}",
            f"--grpc_python_out={HERE}",
            *map(str, proto_files),
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
    pb2      = importlib.import_module("spacex_api.device.device_pb2")
    pb2_grpc = importlib.import_module("spacex_api.device.device_pb2_grpc")
    return pb2, pb2_grpc


# -- Poller --------------------------------------------------------------------

_state: dict = {"data": {}, "error": None, "location_error": None, "last_ok": None}
_lock  = threading.Lock()


def _short_error(exc: Exception) -> str:
    import grpc

    # Extract just the human-readable details from gRPC errors;
    # the full RpcError string is extremely verbose.
    if isinstance(exc, grpc.RpcError):
        return exc.details() or exc.code().name
    return str(exc)


def _snapshot() -> dict:
    with _lock:
        return {
            **_state["data"],
            "error":          _state["error"],
            "location_error": _state["location_error"],
            "last_ok":        _state["last_ok"],
        }


def _set_optional(msg, field: str, value) -> None:
    if value is not None:
        setattr(msg, field, value)


def _status_proto(snapshot: dict) -> ground_station_pb2.StarlinkProxyStatus:
    msg = ground_station_pb2.StarlinkProxyStatus()
    for field in (
        "lat",
        "lon",
        "alt",
        "horizontal_speed_mps",
        "vertical_speed_mps",
        "pop_ping_latency_ms",
        "pop_ping_drop_rate",
        "downlink_throughput_bps",
        "uplink_throughput_bps",
        "boresight_azimuth_deg",
        "boresight_elevation_deg",
        "gps_valid",
        "gps_sats",
        "hardware_version",
        "software_version",
        "uptime_s",
        "error",
        "location_error",
        "last_ok",
    ):
        _set_optional(msg, field, snapshot.get(field))
    return msg


def _poll_loop(pb2, pb2_grpc) -> None:
    import grpc

    channel = grpc.insecure_channel(f"{DISH_HOST}:{DISH_PORT}")
    stub    = pb2_grpc.DeviceStub(channel)

    while True:
        try:
            stat_resp = stub.Handle(pb2.Request(get_status=pb2.GetStatusRequest()))
            stat = stat_resp.dish_get_status

            data = {
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

            location_error = None
            try:
                loc_resp = stub.Handle(pb2.Request(get_location=pb2.GetLocationRequest()))
                loc = loc_resp.get_location
                data.update({
                    "lat":                  loc.lla.lat,
                    "lon":                  loc.lla.lon,
                    "alt":                  loc.lla.alt,
                    "horizontal_speed_mps": loc.horizontal_speed_mps,
                    "vertical_speed_mps":   loc.vertical_speed_mps,
                })
            except Exception as exc:
                location_error = _short_error(exc)
                data.update({
                    "lat":                  None,
                    "lon":                  None,
                    "alt":                  None,
                    "horizontal_speed_mps": None,
                    "vertical_speed_mps":   None,
                })

            with _lock:
                _state["data"]           = data
                _state["error"]          = None
                _state["location_error"] = location_error
                _state["last_ok"]        = time.time()

            if location_error:
                if _state.get("_last_location_error") != location_error:
                    log.warning("location unavailable: %s", location_error)
                    _state["_last_location_error"] = location_error
                log.info("poll ok - location unavailable, latency=%.1f ms",
                         stat.pop_ping_latency_ms)
            else:
                _state["_last_location_error"] = None
                log.info("poll ok - lat=%.4f lon=%.4f latency=%.1f ms",
                         data["lat"], data["lon"], stat.pop_ping_latency_ms)

        except Exception as exc:
            short = _short_error(exc)

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
        if self.path not in ("/starlink", "/starlink/", "/starlink.json", "/starlink.json/"):
            self.send_response(404)
            self.end_headers()
            return

        snapshot = _snapshot()
        wants_json = self.path in ("/starlink.json", "/starlink.json/")

        if wants_json:
            body = json.dumps(snapshot).encode()
            content_type = "application/json"
        else:
            body = _status_proto(snapshot).SerializeToString()
            content_type = "application/x-protobuf"

        self.send_response(200)
        self.send_header("Content-Type",                content_type)
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
