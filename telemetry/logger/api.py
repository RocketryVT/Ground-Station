"""
FastAPI history server — serves logged SQLite data back to the dashboard.

Run alongside the logger:
    uvicorn api:app --port 8000 --reload

Endpoints:
    GET /telemetry?limit=1000&since=<unix_ms>
    GET /antenna?limit=500
    GET /nodes?limit=200
    GET /flights          — list detected flight sessions
    DELETE /flights/last  — wipe the most recent flight from DB
"""

from __future__ import annotations

import sqlite3
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Query
from fastapi.middleware.cors import CORSMiddleware

DB_PATH = Path(__file__).parent / "telemetry.db"

# Import schema from logger so both processes share the same definition.
# If running the API standalone (without the logger), this still creates the DB.
from logger import SCHEMA, init_db  # noqa: E402


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db(DB_PATH)
    yield


app = FastAPI(title="Ground Station History API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET", "DELETE"],
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
    since: Optional[float] = Query(None, description="Unix ms — return rows after this timestamp"),
):
    """Return rocket telemetry rows, oldest first."""
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


# ── Flights ─────────────────────────────────────────────────────────────────────
# A "flight" is a contiguous block of telemetry; gaps > GAP_S seconds separate them.

GAP_S = 30.0

@app.get("/flights")
def list_flights():
    """Detect flight sessions by finding gaps > GAP_S seconds between rows."""
    with db() as conn:
        rows = conn.execute(
            "SELECT timestamp FROM rocket_telemetry ORDER BY timestamp ASC"
        ).fetchall()

    if not rows:
        return []

    timestamps = [r["timestamp"] / 1000.0 for r in rows]  # convert ms → s
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
    """Remove the most recent flight's rows from all tables."""
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
