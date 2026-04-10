# MQTT Flow

## Overview

The primary Pico publishes all telemetry to an MQTT broker on the laptop over
WiFi. This is best-effort and publish-only — the system operates fully without
the laptop connected.

## Data flow

```txt
Rocket
  └─ LoRa ──────────────────────────> Primary Pico  ──> MQTT broker (laptop)
  └─ LoRa ──> Secondary Pico ──UDP──> Primary Pico  ──> MQTT broker (laptop)

Starlink dish ──gRPC──> Primary Pico  (ground GPS + link stats)
Mobile trackers ──cellular MQTT──> broker  (independent observers)
```

## Topic hierarchy

```txt
gs/
├── rocket/
│   ├── telemetry          Decoded rocket telemetry (best received copy)
│   └── event              Flight state transitions
├── radio/
│   ├── primary/link       Per-packet link quality, primary radio
│   ├── secondary/link     Per-packet link quality, secondary radio
│   └── heartbeat          Pre-launch bidirectional link test results
├── pico/
│   ├── primary/
│   │   ├── status         Primary Pico health
│   │   ├── gps            Ground station position (Starlink-sourced)
│   │   └── tracker        Antenna tracker servo state
│   └── secondary/
│       └── status         Secondary Pico health
├── starlink/
│   ├── link               Connectivity metrics (ping, throughput)
│   └── dish               Pointing and obstruction
└── mobile/{id}/
    ├── telemetry          Relayed rocket telemetry (observer's radio)
    └── status             Observer location and device health
```

---

## Topic definitions

### `gs/rocket/telemetry`

Published by the primary Pico each time it receives a decoded LoRa packet
(from either its own radio or forwarded via UDP from the secondary Pico).
`source` indicates which radio path produced this copy.

```json
{
  "t":          1234567890123,
  "boot_ms":    45000,
  "lat":        37.774900,
  "lon":        -122.419400,
  "alt_gps_m":  1200.5,
  "alt_baro_m": 1198.2,
  "speed_ms":   312.4,
  "q":          [0.998, 0.012, -0.034, 0.056],
  "state":      "POWERED_ASCENT",
  "satellites": 9,
  "flags":      7,
  "rssi":       -82,
  "snr":        6.25,
  "source":     "primary"
}
```

Fields:

| Field | Type | Notes |
|---|---|---|
| `t` | uint64 | UTC ms since Unix epoch (from NTP) |
| `boot_ms` | uint32 | Rocket boot time ms (from `Wire::LoRa.boot_ms`) |
| `lat` / `lon` | double | WGS-84 degrees (ROCKET position) |
| `alt_gps_m` | float | GPS altitude, metres MSL |
| `alt_baro_m` | float | Barometric altitude, metres |
| `speed_ms` | float | Scalar speed, m/s |
| `q` | float[4] | Quaternion [w, x, y, z] |
| `state` | string | `FlightState` name |
| `satellites` | uint8 | GPS sats in use |
| `flags` | uint8 | `FLAG_GPS_VALID`=1, `FLAG_BARO_VALID`=2, `FLAG_IMU_VALID`=4 |
| `rssi` | int8 | Receive RSSI, dBm |
| `snr` | float | Receive SNR, dB |
| `source` | string | `"primary"` or `"secondary"` |

---

### `gs/rocket/event`

Published when a `FlightState` transition is detected by the primary Pico.

```json
{
  "t":       1234567890123,
  "event":   "APOGEE",
  "alt_m":   3048.0,
  "boot_ms": 62000
}
```

---

### `gs/radio/primary/link`

Published with every rocket packet received directly by the primary Pico's
radio. Gives a per-packet view of link quality without duplicating telemetry.

```json
{
  "t":        1234567890123,
  "pkt_seq":  42,
  "rssi":     -82,
  "snr":      6.25,
  "freq_mhz": 433.0
}
```

---

### `gs/radio/secondary/link`

Same as `gs/radio/primary/link` but for packets received by the secondary Pico's
radios. Published by the primary Pico after receiving the UDP-forwarded packet.

```json
{
  "t":        1234567890123,
  "pkt_seq":  42,
  "rssi":     -91,
  "snr":      3.75,
  "freq_mhz": 433.0
}
```

---

### `gs/radio/heartbeat`

Published each time the primary Pico receives a heartbeat reply from the rocket
during the pre-launch link test.

```json
{
  "t":          1234567890123,
  "seq":        5,
  "rtt_ms":     124,
  "rssi_gs_rx": -80,
  "snr_gs_rx":  8.5,
  "rssi_rkt_rx": -83,
  "snr_rkt_rx":  7.0
}
```

Fields:

| Field | Notes |
|---|---|
| `rtt_ms` | Round-trip time: `now_ms - received.tx_time_ms` |
| `rssi_gs_rx` / `snr_gs_rx` | What the **ground** heard from the rocket |
| `rssi_rkt_rx` / `snr_rkt_rx` | What the **rocket** heard from the ground (echoed in `rx_rssi` / `rx_snr_q2`) |

---

### `gs/pico/primary/status`

Published by the primary Pico on a slow heartbeat (~10 s).

```json
{
  "t":            1234567890123,
  "uptime_s":     3600,
  "wifi":         true,
  "ntp_synced":   true,
  "udp_rx_count": 150,
  "heap_free":    48200
}
```

---

### `gs/pico/primary/gps`

Ground station position sourced from the Starlink dish gRPC API.
Published when a new fix is obtained (~10 s poll).

```json
{
  "t":         1234567890123,
  "lat":       37.774900,
  "lon":       -122.419400,
  "alt_m":     50.2,
  "speed_mps": 0.1,
  "source":    "GPS"
}
```

`source` is the `PositionSource` enum string from Starlink
(`GPS`, `STARLINK`, `GNC_FUSED`, etc.).

---

### `gs/pico/primary/tracker`

Antenna tracker state, published on change or ~1 s interval.

```json
{
  "t":      1234567890123,
  "az_deg": 270.5,
  "el_deg": 45.2,
  "mode":   "tracking"
}
```

`mode`: `"idle"` | `"tracking"` | `"lost"` | `"manual"`

---

### `gs/pico/secondary/status`

Published by the secondary Pico on a slow heartbeat (~10 s).

```json
{
  "t":            1234567890123,
  "uptime_s":     3600,
  "wifi":         true,
  "udp_tx_count": 150
}
```

---

### `gs/starlink/link`

Starlink connectivity stats from `get_status`. Published ~30 s.

```json
{
  "t":           1234567890123,
  "ping_ms":     45.2,
  "drop_rate":   0.001,
  "dl_mbps":     150.4,
  "ul_mbps":     18.7,
  "gps_valid":   true,
  "gps_sats":    12,
  "uptime_s":    86400
}
```

---

### `gs/starlink/dish`

Dish pointing and health. Published ~30 s or when an alert changes.

```json
{
  "t":                  1234567890123,
  "az_deg":             180.0,
  "el_deg":             62.3,
  "obstructed":         false,
  "fraction_obstructed": 0.02,
  "alerts":             []
}
```

`alerts` is a list of active alert names from `StarlinkAlerts`
(e.g. `["thermal_throttle", "roaming"]`). Empty when healthy.

---

### `gs/mobile/{id}/telemetry`

Relayed rocket telemetry received by a mobile observer via their local LoRa
radio, published to the broker over cellular data.

`{id}` is a short device identifier (e.g. `phone_greg`, `tracker_02`).

```json
{
  "t":       1234567890123,
  "pkt_seq": 42,
  "lat":     37.774900,
  "lon":     -122.419400,
  "alt_m":   1200.5,
  "rssi":    -95,
  "snr":     2.5
}
```

Only includes rocket position fields relevant for a map overlay. Full
telemetry (quaternion, baro alt, etc.) is already in `gs/rocket/telemetry`
from the primary/secondary Picos.

---

### `gs/mobile/{id}/status`

Observer device status, including their current location.

```json
{
  "t":        1234567890123,
  "lat":      37.780000,
  "lon":      -122.410000,
  "battery":  82,
  "cellular": true,
  "lora_rx":  17
}
```

| Field | Notes |
|---|---|
| `lat` / `lon` | **Observer** position (not rocket) |
| `lora_rx` | Packets received from rocket since boot |
