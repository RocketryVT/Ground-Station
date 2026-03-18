// ── Cesium Ion ────────────────────────────────────────────────────────────────
export const CESIUM_ION_TOKEN = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiI4MDc5ZTc3My03NTBjLTRhNDAtODFhNy0wMmY2MmI5NmJhYmQiLCJpZCI6MjYwMTU5LCJpYXQiOjE3Mzg4NzQ0NjN9.OKY5cXo8TzgE6H_tMB84ggCprbZD91TEuBaebhl08wA';

// ── MQTT ──────────────────────────────────────────────────────────────────────
// Mosquitto must have a WebSocket listener enabled (port 9001).
// Add to /etc/mosquitto/mosquitto.conf:
//   listener 9001
//   protocol websockets
export const MQTT_BROKER_URL = 'ws://localhost:9001';

export const TOPICS = {
  ROCKET_TELEMETRY: 'rocket/telemetry',
  ANTENNA_STATE:    'antenna/state',
  NODES_WILDCARD:   'nodes/+/position',
} as const;

// ── History API (Python logger) ───────────────────────────────────────────────
export const API_BASE_URL = 'http://localhost:8000';

// How many telemetry samples to keep in memory (live ring buffer).
// At 10 Hz this is ~5 minutes of data.
export const MAX_HISTORY = 3000;

// ── Antenna beam visualization ────────────────────────────────────────────────
// Adjust BEAM_HALF_ANGLE_DEG to match your actual antenna's half-power beamwidth.
export const BEAM_HALF_ANGLE_DEG = 15;   // degrees — half of total beam width
export const BEAM_RANGE_M        = 4000; // meters  — how far the cone extends
