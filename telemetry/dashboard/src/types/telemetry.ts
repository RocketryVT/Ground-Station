// ── MQTT topic payloads ────────────────────────────────────────────────────────
// These match what the Pico 2W ground station publishes over MQTT.

export interface RocketTelemetry {
  timestamp: number;   // Unix ms
  lat: number;         // decimal degrees
  lon: number;         // decimal degrees
  alt_m: number;       // meters AGL
  vel_n: number;       // m/s north
  vel_e: number;       // m/s east
  vel_d: number;       // m/s down (negative = ascending)
  roll: number;        // degrees
  pitch: number;       // degrees
  yaw: number;         // degrees (0 = north)
  rssi: number;        // dBm
  snr?: number;        // dB
}

export interface AntennaState {
  timestamp: number;
  actual_az: number;  // actual azimuth from encoder  (0 = north, clockwise)
  actual_el: number;  // actual elevation from encoder (0 = horizon, 90 = zenith)
  target_az: number;  // commanded / setpoint azimuth
  target_el: number;  // commanded / setpoint elevation
}

export interface MobileNode {
  id: string;
  lat: number;
  lon: number;
  timestamp: number;
  name?: string;
}
