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
  flap_angle_deg?: number;            // active-drag flap deployment angle
  flap_deployment_percent?: number;   // 0..100, when angle is not sent directly
  target_apogee_m?: number;
  predicted_apogee_m?: number;
  state?: string;                     // firmware flight state (codec flight_state_name)
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
  armed?:           boolean;
  gs_fresh?:        boolean;
  target_fresh?:    boolean;
  ahrs_el_used?:    boolean;
  ahrs_az_used?:    boolean;
  distance_m?:      number;
  pointing_error_az?: number;
  pointing_error_el?: number;
  az_reference_deg?: number;
  el_reference_deg?: number;
  calibration_seq?: number;
  calibration_status?: string;
}

export interface GroundImuState {
  timestamp: number;
  // Bar-frame Euler angles (q_EB — includes elevation tilt, not suitable for azimuth display)
  roll: number;
  pitch: number;
  yaw: number;       // signed, -180..180
  yaw360?: number;   // normalized 0..360
  q?: [number, number, number, number];
  bar_q?: [number, number, number, number];
  yaw_q?: [number, number, number, number];
  bar_rel_q?: [number, number, number, number];
  a?: [number, number, number];
  m?: [number, number, number];
  have_mag?: boolean;
  startup?: boolean;
  mag_rec?: boolean;
  acc_rec?: boolean;
  alt_baro?: number;
  temp?: number;
  valid: boolean;
  // Yaw-platform heading (q_EY from LSM6DSOX+LIS3MDL — tilt-compensated, stable across bar elevation)
  have_yaw_frame?: boolean;
  yaw_frame_yaw?: number;      // signed, -180..180
  yaw_frame_yaw360?: number;   // normalized 0..360 — USE THIS for azimuth display
  yaw_startup?: boolean;
  // Bar orientation relative to yaw platform
  bar_rel_roll?: number;
  bar_rel_pitch?: number;
  bar_rel_yaw?: number;
}

export interface AhrsStatus {
  timestamp: number;
  running: boolean;
  have_imu: boolean;
  have_mag: boolean;
  have_bar_imu?: boolean;
  have_bar_mag?: boolean;
  have_yaw_imu?: boolean;
  have_yaw_mag?: boolean;
  updates: number;
  bar_updates?: number;
  yaw_updates?: number;
}

export interface CalibrationEvent {
  timestamp: number;
  seq?: number;
  action?: string;
  result?: string;
  reference_deg?: number;
  az_calibrated?: boolean;
  el_calibrated?: boolean;
  az_reference_deg?: number;
  el_reference_deg?: number;
  note?: string;
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

// Azimuth bar sensor: LSM6DSOX (IMU) + LIS3MDL (mag) combined publish.
// mag is in µT (sensor native gauss × 100).
export interface RawYawImuSample {
  timestamp: number;
  ax: number; ay: number; az: number;
  gx: number; gy: number; gz: number;
  mx_ut: number; my_ut: number; mz_ut: number;
  temp?: number;
  mag_valid?: boolean;
}

export interface MobileNode {
  id: string;
  lat: number;
  lon: number;
  timestamp: number;
  name?: string;
}
