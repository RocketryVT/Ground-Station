"""
FastAPI history + sim-replay server for the ground station dashboard.

Endpoints:
    GET  /telemetry?limit=1000&since=<unix_ms>
    GET  /antenna?limit=500
    GET  /nodes?limit=200
    GET  /flights
    DELETE /flights/last
    POST /sim/upload          — upload & validate an OpenRocket CSV
    POST /sim/start           — start UDP replay to the primary Pico
    POST /sim/stop            — cancel a running replay
    GET  /sim/status          — poll replay progress
"""

from __future__ import annotations

import asyncio
import math
import socket
import sqlite3
import struct
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Query, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

DB_PATH = Path(__file__).parent / "telemetry.db"

from logger import SCHEMA, init_db  # noqa: E402


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db(DB_PATH)
    yield


app = FastAPI(title="Ground Station History API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "POST", "DELETE"],
    allow_headers=["*"],
)


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(str(DB_PATH))
    conn.row_factory = sqlite3.Row
    return conn


# ── Telemetry ──────────────────────────────────────────────────────────────────

@app.get("/telemetry")
def get_telemetry(
    limit: int = Query(1000, ge=1, le=10_000),
    since: Optional[float] = Query(None),
):
    with db() as conn:
        if since is not None:
            rows = conn.execute(
                "SELECT * FROM rocket_telemetry WHERE timestamp > ? ORDER BY timestamp ASC LIMIT ?",
                (since, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT * FROM rocket_telemetry ORDER BY timestamp ASC LIMIT ?",
                (limit,),
            ).fetchall()
    return [dict(r) for r in rows]


# ── Antenna ────────────────────────────────────────────────────────────────────

@app.get("/antenna")
def get_antenna(limit: int = Query(500, ge=1, le=5_000)):
    with db() as conn:
        rows = conn.execute(
            "SELECT * FROM antenna_state ORDER BY timestamp DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in rows]


# ── Mobile nodes ───────────────────────────────────────────────────────────────

@app.get("/nodes")
def get_nodes(limit: int = Query(200, ge=1, le=2_000)):
    with db() as conn:
        rows = conn.execute(
            "SELECT * FROM mobile_nodes ORDER BY timestamp DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in rows]


# ── Flights ────────────────────────────────────────────────────────────────────

GAP_S = 30.0

@app.get("/flights")
def list_flights():
    with db() as conn:
        rows = conn.execute(
            "SELECT timestamp FROM rocket_telemetry ORDER BY timestamp ASC"
        ).fetchall()

    if not rows:
        return []

    timestamps = [r["timestamp"] / 1000.0 for r in rows]
    flights = []
    start = timestamps[0]
    prev  = timestamps[0]

    for ts in timestamps[1:]:
        if ts - prev > GAP_S:
            flights.append({"start_ms": start * 1000, "end_ms": prev * 1000,
                            "duration_s": round(prev - start, 1)})
            start = ts
        prev = ts

    flights.append({"start_ms": start * 1000, "end_ms": prev * 1000,
                    "duration_s": round(prev - start, 1)})
    return flights


@app.delete("/flights/last")
def delete_last_flight():
    flights = list_flights()
    if not flights:
        return {"deleted": 0}

    last = flights[-1]
    with db() as conn:
        n = conn.execute(
            "DELETE FROM rocket_telemetry WHERE timestamp >= ? AND timestamp <= ?",
            (last["start_ms"], last["end_ms"]),
        ).rowcount
        conn.execute(
            "DELETE FROM antenna_state WHERE timestamp >= ? AND timestamp <= ?",
            (last["start_ms"], last["end_ms"]),
        )
        conn.commit()
    return {"deleted": n, "flight": last}


# ── Sim replay ─────────────────────────────────────────────────────────────────

# SIGMA framing (matches SIGMA.hpp)
_FRAME_START    = bytes([0xAA, 0x55])
_FRAME_END      = bytes([0xBB, 0x66])
_PKT_INTER_PICO = 0x03
_FLAG_GPS_VALID = 0x01
_INTERPICO_FMT  = "<IiiiihhhhHBBBBBBbbBB"
_SIM_RSSI       = -75   # dBm (int8)
_SIM_SNR_Q2     = 20    # 5 dB × 4 (int8)
_SIM_SATS       = 12

# FlightState enum (SIGMA.hpp)
_FS_GROUND_IDLE    = 0
_FS_POWERED_ASCENT = 2
_FS_COAST_ASCENT   = 3
_FS_APOGEE         = 4
_FS_DESCENT_DROGUE = 5
_FS_DESCENT_MAIN   = 6
_FS_LANDED         = 7
_FS_NAMES = {
    0: "GROUND_IDLE", 2: "POWERED_ASCENT", 3: "COAST_ASCENT",
    4: "APOGEE",      5: "DESCENT_DROGUE", 6: "DESCENT_MAIN", 7: "LANDED",
}

_REQUIRED_COL_PATTERNS = [
    "time",
    "altitude above sea level",
    "total velocity",
    "latitude",
    "longitude",
    "roll rate",
    "pitch rate",
    "yaw rate",
]

# Module-level state
_last_upload: dict | None = None
_sim: dict = dict(status="idle", cancel=False, task=None,
                  t=0.0, total_t=0.0, state="", n_sent=0, error=None)


def _crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _build_frame(seq: int, payload: bytes) -> bytes:
    plen   = len(payload)
    header = bytes([_PKT_INTER_PICO, seq & 0xFF, plen & 0xFF, (plen >> 8) & 0xFF])
    crc    = _crc16(header + payload)
    return _FRAME_START + header + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF]) + _FRAME_END


def _qmul(a: tuple, b: tuple) -> tuple:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw*bw - ax*bx - ay*by - az*bz,
            aw*bx + ax*bw + ay*bz - az*by,
            aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw)


def _qnorm(q: tuple) -> tuple:
    n = math.sqrt(sum(x*x for x in q))
    return tuple(x/n for x in q) if n > 1e-10 else (1.0, 0.0, 0.0, 0.0)


def _quat_integrate(q: tuple, roll_dps: float, pitch_dps: float, yaw_dps: float, dt: float) -> tuple:
    wx, wy, wz = math.radians(roll_dps), math.radians(pitch_dps), math.radians(yaw_dps)
    dq = _qmul(q, (0.0, wx, wy, wz))
    h  = 0.5 * dt
    return _qnorm(tuple(q[i] + dq[i] * h for i in range(4)))


def _find_col_idx(headers: list[str], pattern: str) -> int | None:
    for i, h in enumerate(headers):
        if pattern.lower() in h.lower():
            return i
    return None


def _parse_or_csv(text: str) -> tuple[list[str], list[list[float]]]:
    """Parse an OpenRocket CSV.
    The header row is the # comment line with the most comma-separated fields
    (the column-names line), which may be followed by # Event lines.
    """
    lines = text.splitlines()
    best_comment = None
    best_count   = 0
    rows: list[list[float]] = []

    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            candidate = stripped.lstrip("#").strip()
            count = candidate.count(",")
            if count > best_count:
                best_count   = count
                best_comment = candidate
        else:
            parts = stripped.split(",")
            try:
                rows.append([float(p.strip()) for p in parts])
            except ValueError:
                continue

    headers = [h.strip() for h in best_comment.split(",")] if best_comment else []
    return headers, rows


def _flight_state_at(t: float, burnout_t: float, apogee_t: float, land_t: float) -> int:
    if t < 0.05:             return _FS_GROUND_IDLE
    if t < burnout_t:        return _FS_POWERED_ASCENT
    if t < apogee_t - 0.01: return _FS_COAST_ASCENT
    if t < apogee_t + 0.01: return _FS_APOGEE
    if t < land_t - 5.0:    return _FS_DESCENT_DROGUE
    if t < land_t:           return _FS_DESCENT_MAIN
    return _FS_LANDED


async def _run_sim(rows: list, col_idx: dict, pico_ip: str, port: int,
                   speed: float, loop_mode: bool) -> None:
    global _sim

    def gv(row: list, key: str, default: float = 0.0) -> float:
        idx = col_idx.get(key)
        if idx is None or idx >= len(row):
            return default
        return row[idx]

    # Derive event times from the data
    alts     = [gv(r, "altitude above sea level") for r in rows]
    ts_all   = [gv(r, "time") for r in rows]
    apogee_t = ts_all[alts.index(max(alts))]
    land_t   = ts_all[-1]

    # Rough burnout estimate: first local max of total velocity before apogee
    vels = [gv(r, "total velocity") for r in rows]
    burnout_t = apogee_t * 0.15
    for i in range(1, len(vels) - 1):
        if ts_all[i] < apogee_t * 0.5 and vels[i] > vels[i-1] and vels[i] > vels[i+1]:
            burnout_t = ts_all[i]
            break

    _sim["total_t"] = land_t

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        while True:
            _sim["n_sent"] = 0
            q       = (0.0, 0.0, 1.0, 0.0)   # nose-up: rotate 90° around Y
            seq     = 0
            prev_t  = None
            wall_t0 = None
            sim_t0  = gv(rows[0], "time") if rows else 0.0

            for row in rows:
                if _sim["cancel"]:
                    return

                t     = gv(row, "time")
                lat   = gv(row, "latitude")
                lon   = gv(row, "longitude")
                alt   = gv(row, "altitude above sea level")
                spd   = gv(row, "total velocity")
                roll  = gv(row, "roll rate")
                pitch = gv(row, "pitch rate")
                yaw   = gv(row, "yaw rate")

                dt = (t - prev_t) if prev_t is not None else 0.0
                prev_t = t
                if dt > 0:
                    q = _quat_integrate(q, roll, pitch, yaw, dt)

                state = _flight_state_at(t, burnout_t, apogee_t, land_t)
                q16   = [max(-32767, min(32767, int(v * 32767))) for v in q]

                payload = struct.pack(
                    _INTERPICO_FMT,
                    int(t * 1000) & 0xFFFFFFFF,
                    int(lat * 1e7),
                    int(lon * 1e7),
                    int(alt * 10),   # alt_baro_dm
                    int(alt * 100),  # alt_gps_cm
                    q16[0], q16[1], q16[2], q16[3],
                    max(0, min(65535, int(spd * 100))),
                    state & 0xFF, _SIM_SATS, _FLAG_GPS_VALID,
                    0, 0, 0,          # pad[3]
                    _SIM_RSSI, _SIM_SNR_Q2,
                    0, 0,             # pad[2]
                )
                sock.sendto(_build_frame(seq, payload), (pico_ip, port))
                seq = (seq + 1) & 0xFF

                _sim["t"]      = t
                _sim["n_sent"] += 1
                _sim["state"]  = _FS_NAMES.get(state, str(state))

                # Real-time pacing
                loop = asyncio.get_event_loop()
                if wall_t0 is None:
                    wall_t0 = loop.time()
                sleep_needed = wall_t0 + (t - sim_t0) / speed - loop.time()
                if sleep_needed > 0.001:
                    await asyncio.sleep(sleep_needed)

            if not loop_mode:
                break

        _sim["status"] = "done"

    except Exception as exc:
        _sim["status"] = "error"
        _sim["error"]  = str(exc)
    finally:
        sock.close()


class SimStartRequest(BaseModel):
    pico_ip: str
    port:    int   = 5005
    speed:   float = 1.0
    loop:    bool  = False


@app.post("/sim/upload")
async def sim_upload(file: UploadFile):
    global _last_upload

    content = await file.read()
    text    = content.decode("utf-8", errors="replace")
    headers, rows = _parse_or_csv(text)

    found   = headers
    missing = [p for p in _REQUIRED_COL_PATTERNS
               if not any(p.lower() in h.lower() for h in headers)]
    valid   = len(missing) == 0 and len(rows) > 0

    preview = None
    if valid:
        col_idx = {p: _find_col_idx(headers, p) for p in _REQUIRED_COL_PATTERNS}

        def gv(row: list, key: str, default: float = 0.0) -> float:
            idx = col_idx.get(key)
            if idx is None or idx >= len(row): return default
            return row[idx]

        alts       = [gv(r, "altitude above sea level") for r in rows]
        ts_all     = [gv(r, "time") for r in rows]
        launch_alt = alts[0]
        apogee_asl = max(alts)
        apogee_t   = ts_all[alts.index(apogee_asl)]

        preview = {
            "row_count":    len(rows),
            "duration_s":   ts_all[-1],
            "launch_lat":   gv(rows[0], "latitude"),
            "launch_lon":   gv(rows[0], "longitude"),
            "apogee_agl_m": apogee_asl - launch_alt,
            "apogee_asl_m": apogee_asl,
            "events": [
                {"t": ts_all[0],  "name": "LIFTOFF"},
                {"t": apogee_t,   "name": "APOGEE"},
                {"t": ts_all[-1], "name": "GROUND_HIT"},
            ],
        }
        _last_upload = {"rows": rows, "col_idx": col_idx}
    else:
        _last_upload = None

    return {"valid": valid, "found": found, "missing": missing, "preview": preview}


@app.post("/sim/start")
async def sim_start(req: SimStartRequest):
    global _sim

    if _last_upload is None:
        return {"ok": False, "detail": "No valid CSV uploaded"}

    # Cancel any task still running
    if _sim.get("task") and not _sim["task"].done():
        _sim["cancel"] = True
        await asyncio.sleep(0.05)

    _sim = dict(status="running", cancel=False, task=None, t=0.0,
                total_t=0.0, state="GROUND_IDLE", n_sent=0, error=None)

    task = asyncio.create_task(
        _run_sim(_last_upload["rows"], _last_upload["col_idx"],
                 req.pico_ip, req.port, req.speed, req.loop)
    )
    _sim["task"] = task
    return {"ok": True}


@app.post("/sim/stop")
async def sim_stop():
    _sim["cancel"] = True
    return {"ok": True}


@app.get("/sim/status")
def sim_status():
    return {k: v for k, v in _sim.items() if k not in ("task", "cancel")}
