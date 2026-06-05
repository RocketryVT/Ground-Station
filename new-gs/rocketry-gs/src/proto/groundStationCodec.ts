// Protobuf encode/decode for MQTT packets is handled by the Rust backend.
// This file retains only the Starlink proxy decoder used by the debug panel.

type PlainObject = Record<string, unknown>;

const WIRE_VARINT = 0;
const WIRE_64BIT = 1;
const WIRE_LEN = 2;
const WIRE_32BIT = 5;

class Reader {
  pos = 0;

  constructor(private readonly bytes: Uint8Array) {}

  get done(): boolean {
    return this.pos >= this.bytes.length;
  }

  uint(): number {
    let value = 0;
    let mul = 1;
    for (;;) {
      const byte = this.bytes[this.pos++];
      value += (byte & 0x7f) * mul;
      if ((byte & 0x80) === 0) return value;
      mul *= 128;
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
    if (wire === WIRE_VARINT)     this.uint();
    else if (wire === WIRE_64BIT) this.pos += 8;
    else if (wire === WIRE_LEN)   this.pos += this.uint();
    else if (wire === WIRE_32BIT) this.pos += 4;
    else throw new Error(`unsupported protobuf wire type ${wire}`);
  }
}

export function decodeStarlinkProxyStatus(payload: Uint8Array): PlainObject {
  const reader = new Reader(payload);
  const out: PlainObject = {
    lat: null, lon: null, alt: null,
    horizontal_speed_mps: null, vertical_speed_mps: null,
    pop_ping_latency_ms: null, pop_ping_drop_rate: null,
    downlink_throughput_bps: null, uplink_throughput_bps: null,
    boresight_azimuth_deg: null, boresight_elevation_deg: null,
    gps_valid: false, gps_sats: 0,
    hardware_version: null, software_version: null, uptime_s: null,
    error: null, location_error: null, last_ok: null,
  };

  while (!reader.done) {
    const tag = reader.uint();
    const field = tag >>> 3;
    const wire = tag & 7;

    if      (field === 1)  out.lat = reader.double();
    else if (field === 2)  out.lon = reader.double();
    else if (field === 3)  out.alt = reader.double();
    else if (field === 4)  out.horizontal_speed_mps = reader.float();
    else if (field === 5)  out.vertical_speed_mps = reader.float();
    else if (field === 6)  out.pop_ping_latency_ms = reader.float();
    else if (field === 7)  out.pop_ping_drop_rate = reader.float();
    else if (field === 8)  out.downlink_throughput_bps = reader.float();
    else if (field === 9)  out.uplink_throughput_bps = reader.float();
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
