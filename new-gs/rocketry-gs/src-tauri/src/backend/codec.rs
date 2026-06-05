use super::proto;
use super::telemetry::object;
use prost::Message;
use serde_json::{json, Value};

pub const ROCKET_TELEMETRY: &str = "rocket/telemetry";
pub const ROCKET_LORA0: &str = "rocket/lora0";
pub const ROCKET_LORA1_RF69: &str = "rocket/lora1/rf69";
pub const ROCKET_INTER_PICO: &str = "rocket/inter_pico";
pub const ANTENNA_STATE: &str = "antenna/state";
pub const GROUND_IMU: &str = "gs/pico/primary/imu";
pub const AHRS_STATUS: &str = "gs/pico/primary/ahrs/status";
pub const RAW_IMU: &str = "gs/pico/primary/raw/imu";
pub const RAW_MAG: &str = "gs/pico/primary/raw/mag";
pub const RAW_YAW_IMU: &str = "gs/pico/primary/raw/yaw_imu";
pub const GS_LOG: &str = "gs/log";
pub const STEPPER_AZ_CMD: &str = "gs/cmd/az";
pub const STEPPER_EL_CMD: &str = "gs/cmd/zen";
pub const STEPPER_JOG_CMD: &str = "gs/cmd/jog";
pub const RAW_SENSORS_CMD: &str = "gs/cmd/raw_sensors";
pub const DECLINATION_CMD: &str = "gs/cmd/declination";

pub fn decode_topic_payload(topic: &str, payload: &[u8]) -> Option<Result<Value, String>> {
    let decoded = match topic {
        ROCKET_LORA0 | ROCKET_INTER_PICO => decode_rocket_lora(payload),
        ROCKET_LORA1_RF69 => decode_lora1(payload),
        ANTENNA_STATE => decode_antenna(payload),
        GROUND_IMU => decode_ground_imu(payload),
        AHRS_STATUS => decode_ahrs_status(payload),
        RAW_IMU => decode_raw_imu(payload),
        RAW_MAG => decode_raw_mag(payload),
        RAW_YAW_IMU => decode_raw_yaw_imu(payload),
        _ => return None,
    };
    Some(decoded)
}

pub fn decode_to_raw_text(topic: &str, payload: &[u8]) -> (String, Option<Value>) {
    if let Some(result) = decode_topic_payload(topic, payload) {
        return match result {
            Ok(value) => (value.to_string(), Some(value)),
            Err(error) => (
                String::from_utf8_lossy(payload).into_owned(),
                Some(json!({ "error": error })),
            ),
        };
    }

    let text = String::from_utf8_lossy(payload).into_owned();
    let json_value = serde_json::from_str::<Value>(&text).ok();
    (text, json_value)
}

pub fn encode_command_payload(topic: &str, payload: &str) -> Vec<u8> {
    let Ok(data) = serde_json::from_str::<Value>(payload) else {
        return payload.as_bytes().to_vec();
    };

    match topic {
        RAW_SENSORS_CMD => encode_message(proto::RawSensorsCommand {
            imu: bool_field(&data, "imu"),
            mag: bool_field(&data, "mag"),
            yaw_imu: bool_field(&data, "yaw_imu"),
        }),
        STEPPER_AZ_CMD | STEPPER_EL_CMD => encode_message(proto::AxisCommand {
            target_angle_deg: f32_field(&data, "target_angle_deg"),
            speed_dps: f32_field(&data, "speed_dps"),
            stop: bool_field(&data, "stop"),
        }),
        STEPPER_JOG_CMD => encode_message(proto::JogCommand {
            axis: match data.get("axis").and_then(Value::as_str) {
                Some("az") => Some(proto::JogAxis::Az as i32),
                Some("el") | Some("zen") => Some(proto::JogAxis::El as i32),
                _ => None,
            },
            delta_deg: f32_field(&data, "delta_deg"),
            speed_dps: f32_field(&data, "speed_dps"),
        }),
        DECLINATION_CMD => encode_message(proto::DeclinationCommand {
            declination_deg: f32_field(&data, "declination_deg"),
        }),
        _ => payload.as_bytes().to_vec(),
    }
}

pub fn active_drag_from_payload(data: &Value) -> Option<Value> {
    let angle = first_number(
        data,
        &[
            "flap_angle_deg",
            "flap_deployment_angle_deg",
            "active_drag_flap_angle_deg",
            "airbrake_angle_deg",
        ],
    )
    .or_else(|| first_number(data, &["flap_deployment_percent"]).map(|pct| pct * 0.6));
    let deployment_percent = first_number(
        data,
        &[
            "flap_deployment_percent",
            "deployment_percent",
            "deployment_percentage",
            "desired_deployment",
        ],
    );
    let predicted_apogee = first_number(data, &["predicted_apogee_m", "apogee_prediction"]);
    let target_apogee = first_number(data, &["target_apogee_m", "desired_apogee_m"]);

    if angle.is_none()
        && deployment_percent.is_none()
        && predicted_apogee.is_none()
        && target_apogee.is_none()
    {
        return None;
    }

    let mut out = serde_json::Map::new();
    insert_number(&mut out, "flap_angle_deg", angle);
    insert_number(&mut out, "flap_deployment_percent", deployment_percent);
    insert_number(&mut out, "predicted_apogee_m", predicted_apogee);
    insert_number(&mut out, "target_apogee_m", target_apogee);
    Some(Value::Object(out))
}

fn decode_rocket_lora(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::RocketLoRaSample::decode(payload).map_err(|error| error.to_string())?;
    let mut values = serde_json::Map::new();
    insert_u64(&mut values, "boot_ms", msg.boot_ms.map(u64::from));
    if let Some(state) = msg.state {
        values.insert("state".into(), json!(flight_state_name(state)));
    }
    insert_u64(&mut values, "sats", msg.sats.map(u64::from));
    insert_u64(&mut values, "flags", msg.flags.map(u64::from));
    insert_number(&mut values, "lat", msg.lat);
    insert_number(&mut values, "lon", msg.lon);
    insert_number(&mut values, "alt_gps_m", msg.alt_gps_m.map(f64::from));
    insert_number(&mut values, "alt_baro_m", msg.alt_baro_m.map(f64::from));
    insert_number(&mut values, "speed_ms", msg.speed_ms.map(f64::from));
    if !msg.q.is_empty() {
        values.insert("q".into(), json!(msg.q));
    }
    insert_number(&mut values, "rssi", msg.rssi.map(f64::from));
    insert_number(&mut values, "snr", msg.snr.map(f64::from));
    Ok(Value::Object(values))
}

fn decode_lora1(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::Lora1Rf69Packet::decode(payload).map_err(|error| error.to_string())?;
    let data = msg.data.unwrap_or_default();
    Ok(object([
        ("data", json!(hex(&data))),
        ("len", json!(data.len())),
        ("rssi", json!(msg.rssi)),
        ("snr", json!(msg.snr)),
    ]))
}

fn decode_antenna(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::AntennaState::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("actual_az", json!(msg.actual_az)),
        ("actual_el", json!(msg.actual_el)),
        ("target_az", json!(msg.target_az)),
        ("target_el", json!(msg.target_el)),
        ("actual_az_mech", json!(msg.actual_az_mech)),
        ("target_az_mech", json!(msg.target_az_mech)),
        ("az_calibrated", json!(msg.az_calibrated)),
        ("zen_calibrated", json!(msg.zen_calibrated)),
        ("tracking_enabled", json!(msg.tracking_enabled)),
        ("az_moving", json!(msg.az_moving)),
        ("zen_moving", json!(msg.zen_moving)),
        ("az_faulted", json!(msg.az_faulted)),
        ("zen_faulted", json!(msg.zen_faulted)),
        ("mode", json!(msg.mode)),
    ]))
}

fn decode_ground_imu(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::GroundImu::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("roll", json!(msg.roll)),
        ("pitch", json!(msg.pitch)),
        ("yaw", json!(msg.yaw)),
        ("yaw360", json!(msg.yaw360)),
        (
            "q",
            json!(if msg.q.is_empty() { None } else { Some(msg.q) }),
        ),
        (
            "bar_q",
            json!(if msg.bar_q.is_empty() {
                None
            } else {
                Some(msg.bar_q)
            }),
        ),
        (
            "yaw_q",
            json!(if msg.yaw_q.is_empty() {
                None
            } else {
                Some(msg.yaw_q)
            }),
        ),
        (
            "bar_rel_q",
            json!(if msg.bar_rel_q.is_empty() {
                None
            } else {
                Some(msg.bar_rel_q)
            }),
        ),
        (
            "a",
            json!(if msg.a.is_empty() { None } else { Some(msg.a) }),
        ),
        (
            "m",
            json!(if msg.m.is_empty() { None } else { Some(msg.m) }),
        ),
        ("have_mag", json!(msg.have_mag)),
        ("startup", json!(msg.startup)),
        ("mag_rec", json!(msg.mag_rec)),
        ("acc_rec", json!(msg.acc_rec)),
        ("alt_baro", json!(msg.alt_baro)),
        ("temp", json!(msg.temp)),
        ("valid", json!(msg.valid.unwrap_or(false))),
        ("have_yaw_frame", json!(msg.have_yaw_frame)),
        ("yaw_frame_yaw", json!(msg.yaw_frame_yaw)),
        ("yaw_frame_yaw360", json!(msg.yaw_frame_yaw360)),
        ("yaw_startup", json!(msg.yaw_startup)),
        ("bar_rel_roll", json!(msg.bar_rel_roll)),
        ("bar_rel_pitch", json!(msg.bar_rel_pitch)),
        ("bar_rel_yaw", json!(msg.bar_rel_yaw)),
    ]))
}

fn decode_ahrs_status(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::AhrsStatus::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("running", json!(msg.running.unwrap_or(false))),
        ("have_bar_imu", json!(msg.have_bar_imu.unwrap_or(false))),
        ("have_bar_mag", json!(msg.have_bar_mag.unwrap_or(false))),
        ("have_yaw_imu", json!(msg.have_yaw_imu.unwrap_or(false))),
        ("have_yaw_mag", json!(msg.have_yaw_mag.unwrap_or(false))),
        ("bar_updates", json!(msg.bar_updates.unwrap_or(0))),
        ("yaw_updates", json!(msg.yaw_updates.unwrap_or(0))),
        ("have_imu", json!(msg.have_bar_imu.unwrap_or(false))),
        ("have_mag", json!(msg.have_bar_mag.unwrap_or(false))),
        (
            "updates",
            json!(msg
                .bar_updates
                .unwrap_or(0)
                .max(msg.yaw_updates.unwrap_or(0))),
        ),
    ]))
}

fn decode_raw_imu(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::RawImuSample::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("ax", json!(msg.ax)),
        ("ay", json!(msg.ay)),
        ("az", json!(msg.az)),
        ("gx", json!(msg.gx)),
        ("gy", json!(msg.gy)),
        ("gz", json!(msg.gz)),
        ("temp", json!(msg.temp)),
    ]))
}

fn decode_raw_mag(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::RawMagSample::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("mx", json!(msg.mx)),
        ("my", json!(msg.my)),
        ("mz", json!(msg.mz)),
    ]))
}

fn decode_raw_yaw_imu(payload: &[u8]) -> Result<Value, String> {
    let msg = proto::RawYawImuSample::decode(payload).map_err(|error| error.to_string())?;
    Ok(object([
        ("timestamp", json!(msg.timestamp)),
        ("ax", json!(msg.ax)),
        ("ay", json!(msg.ay)),
        ("az", json!(msg.az)),
        ("gx", json!(msg.gx)),
        ("gy", json!(msg.gy)),
        ("gz", json!(msg.gz)),
        ("mx_ut", json!(msg.mx_ut)),
        ("my_ut", json!(msg.my_ut)),
        ("mz_ut", json!(msg.mz_ut)),
        ("mag_valid", json!(msg.mag_valid)),
        ("mag_overflow", json!(msg.mag_overflow)),
        ("temp", json!(msg.temp)),
    ]))
}

fn encode_message(message: impl Message) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(message.encoded_len());
    message.encode(&mut bytes).expect("encode protobuf command");
    bytes
}

fn bool_field(data: &Value, key: &str) -> Option<bool> {
    data.get(key).and_then(Value::as_bool)
}

fn f32_field(data: &Value, key: &str) -> Option<f32> {
    data.get(key)
        .and_then(Value::as_f64)
        .filter(|value| value.is_finite())
        .map(|value| value as f32)
}

fn first_number(data: &Value, keys: &[&str]) -> Option<f64> {
    keys.iter()
        .find_map(|key| data.get(*key).and_then(Value::as_f64))
        .filter(|value| value.is_finite())
}

fn insert_number(map: &mut serde_json::Map<String, Value>, key: &str, value: Option<f64>) {
    if let Some(value) = value {
        map.insert(key.to_string(), json!(value));
    }
}

fn insert_u64(map: &mut serde_json::Map<String, Value>, key: &str, value: Option<u64>) {
    if let Some(value) = value {
        map.insert(key.to_string(), json!(value));
    }
}

fn flight_state_name(value: i32) -> &'static str {
    match value {
        0 => "GROUND_IDLE",
        1 => "ARMED",
        2 => "POWERED_ASCENT",
        3 => "COAST_ASCENT",
        4 => "APOGEE",
        5 => "DESCENT_DROGUE",
        6 => "DESCENT_MAIN",
        7 => "LANDED",
        255 => "FAULT",
        _ => "UNKNOWN",
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02X}")).collect()
}
