// -- MQTT topic payloads --------------------------------------------------------
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
  timestamp:        number;
  actual_az:        number;   // actual azimuth (0 = north, clockwise)
  actual_el:        number;   // actual elevation (0 = horizon, 90 = zenith)
  target_az:        number;
  target_el:        number;
  actual_az_mech?:  number;   // raw mechanical azimuth (before north offset)
  target_az_mech?:  number;
  az_calibrated?:   boolean;
  zen_calibrated?:  boolean;
  tracking_enabled?: boolean;
  mode?:            string;
  az_moving?:       boolean;
  zen_moving?:      boolean;
  az_faulted?:      boolean;
  zen_faulted?:     boolean;
}

export interface GroundImuState {
  timestamp: number;
  roll: number;
  pitch: number;
  yaw: number;       // signed Fusion Euler yaw, -180..180
  yaw360?: number;   // normalized yaw, 0..360
  q?: [number, number, number, number];
  a?: [number, number, number];
  m?: [number, number, number];
  have_mag?: boolean;
  startup?: boolean;
  mag_rec?: boolean;
  acc_rec?: boolean;
  alt_baro?: number;
  temp?: number;
  valid: boolean;
}

export interface RawImuSample {
  timestamp: number;
  ax: number;
  ay: number;
  az: number;
  gx: number;
  gy: number;
  gz: number;
  temp?: number;
}

export interface RawMagSample {
  timestamp: number;
  mx: number;
  my: number;
  mz: number;
}

export interface MobileNode {
  id: string;
  lat: number;
  lon: number;
  timestamp: number;
  name?: string;
}
