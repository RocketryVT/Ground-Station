"""
MQTT -> SQLite logger
Subscribes to the same topics as the dashboard and persists every packet.

Run alongside the dashboard:
    python logger.py
"""

import json
import sqlite3
import signal
import sys
from pathlib import Path

import paho.mqtt.client as mqtt

# -- Config ---------------------------------------------------------------------
BROKER_HOST = "localhost"
BROKER_PORT  = 1883
DB_PATH      = Path(__file__).parent / "telemetry.db"

TOPICS = [
    "rocket/telemetry",
    "antenna/state",
    "nodes/+/position",
]

# -- Schema ---------------------------------------------------------------------
SCHEMA = """
CREATE TABLE IF NOT EXISTS rocket_telemetry (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   REAL NOT NULL,
    lat         REAL, lon        REAL, alt_m      REAL,
    vel_n       REAL, vel_e      REAL, vel_d       REAL,
    roll        REAL, pitch      REAL, yaw         REAL,
    rssi        REAL, snr        REAL
);
CREATE INDEX IF NOT EXISTS idx_rt_ts ON rocket_telemetry(timestamp);

CREATE TABLE IF NOT EXISTS antenna_state (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   REAL NOT NULL,
    zenith_deg  REAL, azimuth_deg REAL,
    target_az   REAL, target_el   REAL
);
CREATE INDEX IF NOT EXISTS idx_ant_ts ON antenna_state(timestamp);

CREATE TABLE IF NOT EXISTS mobile_nodes (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id     TEXT NOT NULL,
    timestamp   REAL NOT NULL,
    lat         REAL, lon REAL,
    name        TEXT
);
CREATE INDEX IF NOT EXISTS idx_node_ts ON mobile_nodes(timestamp);
"""

def init_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.executescript(SCHEMA)
    conn.commit()
    return conn

# -- Message handler -------------------------------------------------------------
def make_on_message(conn: sqlite3.Connection):
    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode())
        except Exception as e:
            print(f"[WARN] bad payload on {msg.topic}: {e}")
            return

        topic = msg.topic

        try:
            if topic == "rocket/telemetry":
                conn.execute(
                    """INSERT INTO rocket_telemetry
                       (timestamp, lat, lon, alt_m, vel_n, vel_e, vel_d,
                        roll, pitch, yaw, rssi, snr)
                       VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (
                        data.get("timestamp"), data.get("lat"),  data.get("lon"),
                        data.get("alt_m"),     data.get("vel_n"), data.get("vel_e"),
                        data.get("vel_d"),     data.get("roll"),  data.get("pitch"),
                        data.get("yaw"),       data.get("rssi"),  data.get("snr"),
                    ),
                )

            elif topic == "antenna/state":
                conn.execute(
                    """INSERT INTO antenna_state
                       (timestamp, zenith_deg, azimuth_deg, target_az, target_el)
                       VALUES (?,?,?,?,?)""",
                    (
                        data.get("timestamp"),   data.get("zenith_deg"),
                        data.get("azimuth_deg"), data.get("target_az"),
                        data.get("target_el"),
                    ),
                )

            elif topic.startswith("nodes/"):
                node_id = topic.split("/")[1]
                conn.execute(
                    """INSERT INTO mobile_nodes (node_id, timestamp, lat, lon, name)
                       VALUES (?,?,?,?,?)""",
                    (
                        node_id,            data.get("timestamp"),
                        data.get("lat"),    data.get("lon"),
                        data.get("name"),
                    ),
                )

            conn.commit()

        except Exception as e:
            print(f"[ERROR] DB write on {topic}: {e}")

    return on_message


def main():
    conn = init_db(DB_PATH)
    print(f"[logger] DB: {DB_PATH}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = make_on_message(conn)

    client.on_connect    = lambda c, u, f, r, p: (
        print(f"[logger] connected, rc={r}"),
        [c.subscribe(t) for t in TOPICS],
    )
    client.on_disconnect = lambda c, u, d, r, p: print(f"[logger] disconnected rc={r}")

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)

    def _shutdown(sig, frame):
        print("\n[logger] shutting down")
        client.disconnect()
        conn.close()
        sys.exit(0)

    signal.signal(signal.SIGINT,  _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    print(f"[logger] listening on {BROKER_HOST}:{BROKER_PORT}")
    client.loop_forever()


if __name__ == "__main__":
    main()
