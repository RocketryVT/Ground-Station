import { TOPICS } from '../config';

type PlainObject = Record<string, unknown>;

const WIRE_VARINT = 0;
const WIRE_64BIT = 1;
const WIRE_LEN = 2;
const WIRE_32BIT = 5;

const flightStates: Record<number, string> = {
  0: 'GROUND_IDLE',
  1: 'ARMED',
  2: 'POWERED_ASCENT',
  3: 'COAST_ASCENT',
  4: 'APOGEE',
  5: 'DESCENT_DROGUE',
  6: 'DESCENT_MAIN',
  7: 'LANDED',
  255: 'FAULT',
};

class Reader {
  pos = 0;

  constructor(private readonly bytes: Uint8Array) {}

  get done(): boolean {
    return this.pos >= this.bytes.length;
  }

  uint(): number {
    let value = 0;
    let shift = 0;
    let mul = 1;

    for (;;) {
      const byte = this.bytes[this.pos++];
      value += (byte & 0x7f) * mul;
      if ((byte & 0x80) === 0) return value;
      shift += 7;
      mul = 2 ** shift;
    }
  }

  bool(): boolean {
    return this.uint() !== 0;
  }

  float(): number {
    const view = new DataView(this.bytes.buffer, this.bytes.byteOffset + this.pos, 4);
    this.pos += 4;
    return view.getFloat32(0, true);
  }

  double(): number {
    const view = new DataView(this.bytes.buffer, this.bytes.byteOffset + this.pos, 8);
    this.pos += 8;
    return view.getFloat64(0, true);
  }

  bytesField(): Uint8Array {
    const len = this.uint();
    const start = this.pos;
    this.pos += len;
    return this.bytes.slice(start, start + len);
  }

  string(): string {
    return new TextDecoder().decode(this.bytesField());
  }

  skip(wire: number): void {
    if (wire === WIRE_VARINT) {
      this.uint();
    } else if (wire === WIRE_64BIT) {
      this.pos += 8;
    } else if (wire === WIRE_LEN) {
      this.pos += this.uint();
    } else if (wire === WIRE_32BIT) {
      this.pos += 4;
    } else {
      throw new Error(`unsupported protobuf wire type ${wire}`);
    }
  }
}

class Writer {
  private out: number[] = [];

  tag(field: number, wire: number): void {
    this.uint((field << 3) | wire);
  }

  uint(value: number): void {
    let next = Math.max(0, Math.floor(value));
    while (next > 0x7f) {
      this.out.push((next & 0x7f) | 0x80);
      next = Math.floor(next / 128);
    }
    this.out.push(next);
  }

  bool(field: number, value: boolean): void {
    this.tag(field, WIRE_VARINT);
    this.uint(value ? 1 : 0);
  }

  enum(field: number, value: number): void {
    this.tag(field, WIRE_VARINT);
    this.uint(value);
  }

  float(field: number, value: number): void {
    this.tag(field, WIRE_32BIT);
    const bytes = new Uint8Array(4);
    new DataView(bytes.buffer).setFloat32(0, value, true);
    this.out.push(...bytes);
  }

  finish(): Uint8Array {
    return new Uint8Array(this.out);
  }
}

function readFloatRepeated(reader: Reader, wire: number): number[] {
  if (wire === WIRE_32BIT) return [reader.float()];
  if (wire !== WIRE_LEN) throw new Error(`bad repeated float wire ${wire}`);

  const len = reader.uint();
  const end = reader.pos + len;
  const values: number[] = [];
  while (reader.pos < end) values.push(reader.float());
  return values;
}

function hex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0').toUpperCase()).join('');
}

function decodeRocketLoRa(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};
  const q: number[] = [];

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.boot_ms = reader.uint();
    else if (field === 2) out.state = flightStates[reader.uint()] ?? 'UNKNOWN';
    else if (field === 3) out.sats = reader.uint();
    else if (field === 4) out.flags = reader.uint();
    else if (field === 5) out.lat = reader.double();
    else if (field === 6) out.lon = reader.double();
    else if (field === 7) out.alt_gps_m = reader.float();
    else if (field === 8) out.alt_baro_m = reader.float();
    else if (field === 9) out.speed_ms = reader.float();
    else if (field === 10) q.push(...readFloatRepeated(reader, wire));
    else if (field === 11) out.rssi = reader.float();
    else if (field === 12) out.snr = reader.float();
    else reader.skip(wire);
  }

  if (q.length > 0) out.q = q;
  return out;
}

function decodeLora1(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) {
      const data = reader.bytesField();
      out.data = hex(data);
      out.len = data.length;
    } else if (field === 2) out.rssi = reader.float();
    else if (field === 3) out.snr = reader.float();
    else reader.skip(wire);
  }

  return out;
}

function decodeAntenna(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.actual_az = reader.float();
    else if (field === 3) out.actual_el = reader.float();
    else if (field === 4) out.target_az = reader.float();
    else if (field === 5) out.target_el = reader.float();
    else if (field === 6) out.actual_az_mech = reader.float();
    else if (field === 7) out.target_az_mech = reader.float();
    else if (field === 8) out.az_calibrated = reader.bool();
    else if (field === 9) out.zen_calibrated = reader.bool();
    else if (field === 10) out.tracking_enabled = reader.bool();
    else if (field === 11) out.az_moving = reader.bool();
    else if (field === 12) out.zen_moving = reader.bool();
    else if (field === 13) out.az_faulted = reader.bool();
    else if (field === 14) out.zen_faulted = reader.bool();
    else if (field === 15) out.mode = reader.string();
    else reader.skip(wire);
  }

  return out;
}

function decodeGroundImu(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};
  const q: number[] = [];
  const barQ: number[] = [];
  const yawQ: number[] = [];
  const barRelQ: number[] = [];
  const a: number[] = [];
  const m: number[] = [];

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.roll = reader.float();
    else if (field === 3) out.pitch = reader.float();
    else if (field === 4) out.yaw = reader.float();
    else if (field === 5) out.yaw360 = reader.float();
    else if (field === 6) q.push(...readFloatRepeated(reader, wire));
    else if (field === 7) a.push(...readFloatRepeated(reader, wire));
    else if (field === 8) m.push(...readFloatRepeated(reader, wire));
    else if (field === 9) out.have_mag = reader.bool();
    else if (field === 10) out.startup = reader.bool();
    else if (field === 11) out.mag_rec = reader.bool();
    else if (field === 12) out.acc_rec = reader.bool();
    else if (field === 13) out.alt_baro = reader.float();
    else if (field === 14) out.temp = reader.float();
    else if (field === 15) out.valid = reader.bool();
    else if (field === 16) out.have_yaw_frame = reader.bool();
    else if (field === 17) out.yaw_frame_yaw = reader.float();
    else if (field === 18) out.yaw_frame_yaw360 = reader.float();
    else if (field === 19) out.yaw_startup = reader.bool();
    else if (field === 20) out.bar_rel_roll = reader.float();
    else if (field === 21) out.bar_rel_pitch = reader.float();
    else if (field === 22) out.bar_rel_yaw = reader.float();
    else if (field === 23) barQ.push(...readFloatRepeated(reader, wire));
    else if (field === 24) yawQ.push(...readFloatRepeated(reader, wire));
    else if (field === 25) barRelQ.push(...readFloatRepeated(reader, wire));
    else reader.skip(wire);
  }

  if (q.length > 0) out.q = q;
  if (barQ.length > 0) out.bar_q = barQ;
  if (yawQ.length > 0) out.yaw_q = yawQ;
  if (barRelQ.length > 0) out.bar_rel_q = barRelQ;
  if (a.length > 0) out.a = a;
  if (m.length > 0) out.m = m;
  return out;
}

function decodeAhrsStatus(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.running = reader.bool();
    else if (field === 3) out.have_bar_imu = reader.bool();
    else if (field === 4) out.have_bar_mag = reader.bool();
    else if (field === 5) out.have_yaw_imu = reader.bool();
    else if (field === 6) out.have_yaw_mag = reader.bool();
    else if (field === 7) out.bar_updates = reader.uint();
    else if (field === 8) out.yaw_updates = reader.uint();
    else reader.skip(wire);
  }

  out.have_imu = Boolean(out.have_bar_imu);
  out.have_mag = Boolean(out.have_bar_mag);
  out.updates = Math.max(Number(out.bar_updates ?? 0), Number(out.yaw_updates ?? 0));
  return out;
}

function decodeRawImu(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.ax = reader.float();
    else if (field === 3) out.ay = reader.float();
    else if (field === 4) out.az = reader.float();
    else if (field === 5) out.gx = reader.float();
    else if (field === 6) out.gy = reader.float();
    else if (field === 7) out.gz = reader.float();
    else if (field === 8) out.temp = reader.float();
    else reader.skip(wire);
  }

  return out;
}

function decodeRawMag(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.mx = reader.float();
    else if (field === 3) out.my = reader.float();
    else if (field === 4) out.mz = reader.float();
    else reader.skip(wire);
  }

  return out;
}

function decodeRawYawImu(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {};

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.timestamp = reader.uint();
    else if (field === 2) out.ax = reader.float();
    else if (field === 3) out.ay = reader.float();
    else if (field === 4) out.az = reader.float();
    else if (field === 5) out.gx = reader.float();
    else if (field === 6) out.gy = reader.float();
    else if (field === 7) out.gz = reader.float();
    else if (field === 8) out.mx_ut = reader.float();
    else if (field === 9) out.my_ut = reader.float();
    else if (field === 10) out.mz_ut = reader.float();
    else if (field === 11) out.mag_valid = reader.bool();
    else if (field === 12) out.mag_overflow = reader.bool();
    else if (field === 13) out.temp = reader.float();
    else reader.skip(wire);
  }

  return out;
}

export function decodeTopicPayload(topic: string, payload: Uint8Array): PlainObject | null {
  switch (topic) {
    case TOPICS.ROCKET_LORA0:
    case TOPICS.ROCKET_INTER_PICO:
      return decodeRocketLoRa(payload);
    case TOPICS.ROCKET_LORA1_RF69:
      return decodeLora1(payload);
    case TOPICS.ANTENNA_STATE:
      return decodeAntenna(payload);
    case TOPICS.GROUND_IMU:
      return decodeGroundImu(payload);
    case TOPICS.AHRS_STATUS:
      return decodeAhrsStatus(payload);
    case TOPICS.RAW_IMU:
      return decodeRawImu(payload);
    case TOPICS.RAW_MAG:
      return decodeRawMag(payload);
    case TOPICS.RAW_YAW_IMU:
      return decodeRawYawImu(payload);
    default:
      return null;
  }
}

export function decodeStarlinkProxyStatus(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {
    lat: null,
    lon: null,
    alt: null,
    horizontal_speed_mps: null,
    vertical_speed_mps: null,
    pop_ping_latency_ms: null,
    pop_ping_drop_rate: null,
    downlink_throughput_bps: null,
    uplink_throughput_bps: null,
    boresight_azimuth_deg: null,
    boresight_elevation_deg: null,
    gps_valid: false,
    gps_sats: 0,
    hardware_version: null,
    software_version: null,
    uptime_s: null,
    error: null,
    location_error: null,
    last_ok: null,
  };

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if (field === 1) out.lat = reader.double();
    else if (field === 2) out.lon = reader.double();
    else if (field === 3) out.alt = reader.double();
    else if (field === 4) out.horizontal_speed_mps = reader.float();
    else if (field === 5) out.vertical_speed_mps = reader.float();
    else if (field === 6) out.pop_ping_latency_ms = reader.float();
    else if (field === 7) out.pop_ping_drop_rate = reader.float();
    else if (field === 8) out.downlink_throughput_bps = reader.float();
    else if (field === 9) out.uplink_throughput_bps = reader.float();
    else if (field === 10) out.boresight_azimuth_deg = reader.float();
    else if (field === 11) out.boresight_elevation_deg = reader.float();
    else if (field === 12) out.gps_valid = reader.bool();
    else if (field === 13) out.gps_sats = reader.uint();
    else if (field === 14) out.hardware_version = reader.string();
    else if (field === 15) out.software_version = reader.string();
    else if (field === 16) out.uptime_s = reader.uint();
    else if (field === 17) out.error = reader.string();
    else if (field === 18) out.location_error = reader.string();
    else if (field === 19) out.last_ok = reader.double();
    else reader.skip(wire);
  }

  return out;
}

function commandObject(payload: string | Uint8Array): PlainObject | null {
  if (payload instanceof Uint8Array) return null;
  try {
    return JSON.parse(payload) as PlainObject;
  } catch {
    return null;
  }
}

function finiteNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function maybeBool(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : undefined;
}

function encodeRawSensorsCommand(data: PlainObject): Uint8Array {
  const writer = new Writer();
  const imu = maybeBool(data.imu);
  const mag = maybeBool(data.mag);
  const yawImu = maybeBool(data.yaw_imu);
  if (imu != null) writer.bool(1, imu);
  if (mag != null) writer.bool(2, mag);
  if (yawImu != null) writer.bool(3, yawImu);
  return writer.finish();
}

function encodeAxisCommand(data: PlainObject): Uint8Array {
  const writer = new Writer();
  const target = finiteNumber(data.target_angle_deg);
  const speed = finiteNumber(data.speed_dps);
  const stop = maybeBool(data.stop);
  if (target != null) writer.float(1, target);
  if (speed != null) writer.float(2, speed);
  if (stop != null) writer.bool(3, stop);
  return writer.finish();
}

function encodeJogCommand(data: PlainObject): Uint8Array {
  const writer = new Writer();
  const axis = data.axis === 'az' ? 1 : data.axis === 'el' || data.axis === 'zen' ? 2 : 0;
  const delta = finiteNumber(data.delta_deg);
  const speed = finiteNumber(data.speed_dps);
  if (axis !== 0) writer.enum(1, axis);
  if (delta != null) writer.float(2, delta);
  if (speed != null) writer.float(3, speed);
  return writer.finish();
}

function encodeDeclinationCommand(data: PlainObject): Uint8Array {
  const writer = new Writer();
  const declination = finiteNumber(data.declination_deg);
  if (declination != null) writer.float(1, declination);
  return writer.finish();
}

export function encodeCommandPayload(topic: string, payload: string | Uint8Array): string | Uint8Array {
  if (payload instanceof Uint8Array) return payload;

  const data = commandObject(payload);
  if (!data) return payload;

  switch (topic) {
    case TOPICS.RAW_SENSORS_CMD:
      return encodeRawSensorsCommand(data);
    case TOPICS.STEPPER_AZ_CMD:
    case TOPICS.STEPPER_EL_CMD:
      return encodeAxisCommand(data);
    case TOPICS.STEPPER_JOG_CMD:
      return encodeJogCommand(data);
    case TOPICS.DECLINATION_CMD:
      return encodeDeclinationCommand(data);
    default:
      return payload;
  }
}
