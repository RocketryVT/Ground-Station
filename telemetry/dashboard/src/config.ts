// -- Cesium Ion ----------------------------------------------------------------
export const CESIUM_ION_TOKEN = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiI4MDc5ZTc3My03NTBjLTRhNDAtODFhNy0wMmY2MmI5NmJhYmQiLCJpZCI6MjYwMTU5LCJpYXQiOjE3Mzg4NzQ0NjN9.OKY5cXo8TzgE6H_tMB84ggCprbZD91TEuBaebhl08wA';

// -- MQTT ----------------------------------------------------------------------
// Mosquitto must have a WebSocket listener enabled (port 9001).
// Add to /etc/mosquitto/mosquitto.conf:
//   listener 9001
//   protocol websockets
export const MQTT_BROKER_URL = 'ws://localhost:9001';

export const TOPICS = {
  ROCKET_TELEMETRY:  'rocket/telemetry',
  ANTENNA_STATE:     'antenna/state',
  GROUND_IMU:        'gs/pico/primary/imu',
  AHRS_STATUS:       'gs/pico/primary/ahrs/status',
  RAW_IMU:           'gs/pico/primary/raw/imu',
  RAW_MAG:           'gs/pico/primary/raw/mag',
  NODES_WILDCARD:    'nodes/+/position',
  GS_LOG:            'gs/log',
  STEPPER_AZ_CMD:    'gs/cmd/az',
  STEPPER_ZEN_CMD:   'gs/cmd/zen',
  STEPPER_JOG_CMD:   'gs/cmd/jog',
  CALIBRATION_CMD:   'gs/cmd/calibration',
  RAW_SENSORS_CMD:   'gs/cmd/raw_sensors',
  DEBUG_AZ_OSC:      'gs/cmd/debug/az',
  DEBUG_ZEN_OSC:     'gs/cmd/debug/zen',
} as const;

// -- History API (Python logger) -----------------------------------------------
export const API_BASE_URL = 'http://localhost:8000';

// -- Starlink proxy (starlink_proxy.py, polls dish at 192.168.100.1:9200) -----
export const STARLINK_PROXY_URL = 'http://localhost:8001/starlink';

// How many live telemetry samples to keep in memory per streamed MQTT history.
export const MAX_HISTORY = 500;

// -- Antenna beam visualization ------------------------------------------------
// Adjust BEAM_HALF_ANGLE_DEG to match your actual antenna's half-power beamwidth.
export const BEAM_HALF_ANGLE_DEG = 15;   // degrees — half of total beam width
export const BEAM_RANGE_M        = 4000; // meters  — how far the cone extends
