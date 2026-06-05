use super::telemetry::{now_ms, TelemetryState};
use rusqlite::{params, Connection};
use serde_json::Value;
use std::fs::{metadata, File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::Path;
use std::sync::{Arc, Mutex};

#[derive(Clone)]
pub struct PacketLogger {
    conn: Arc<Mutex<Connection>>,
}

impl PacketLogger {
    pub fn open(path: &Path) -> Result<Self, Box<dyn std::error::Error>> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }

        let conn = Connection::open(path)?;
        conn.pragma_update(None, "journal_mode", "WAL")?;
        conn.pragma_update(None, "synchronous", "NORMAL")?;
        conn.execute_batch(
            "
            CREATE TABLE IF NOT EXISTS packet_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts_ms INTEGER NOT NULL,
                direction TEXT NOT NULL CHECK(direction IN ('rx', 'tx')),
                topic TEXT NOT NULL,
                payload BLOB NOT NULL,
                decoded_text TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_packet_log_ts ON packet_log(ts_ms);
            CREATE INDEX IF NOT EXISTS idx_packet_log_topic ON packet_log(topic);
            ",
        )?;

        Ok(Self {
            conn: Arc::new(Mutex::new(conn)),
        })
    }

    pub fn log_packet(
        &self,
        direction: &str,
        topic: &str,
        payload: &[u8],
        decoded_text: Option<&str>,
    ) {
        let result = self.conn.lock().expect("packet logger").execute(
            "INSERT INTO packet_log (ts_ms, direction, topic, payload, decoded_text)
             VALUES (?, ?, ?, ?, ?)",
            params![now_ms(), direction, topic, payload, decoded_text],
        );
        if let Err(error) = result {
            eprintln!("[packet-log] {direction} {topic}: {error}");
        }
    }
}

#[derive(Clone)]
pub struct DiagnosticCsv {
    writer: Arc<Mutex<BufWriter<File>>>,
}

impl DiagnosticCsv {
    pub fn log_event(
        &self,
        event: &str,
        direction: &str,
        topic: &str,
        payload: &str,
        state: &TelemetryState,
    ) {
        let snapshot = state.snapshot();
        let row = DiagnosticRow::from_event(event, direction, topic, payload, &snapshot);
        let mut writer = self.writer.lock().expect("diagnostic csv");

        if let Err(error) = writeln!(writer, "{}", row.to_csv()) {
            eprintln!("[diagnostic-csv] write failed: {error}");
            return;
        }
        if let Err(error) = writer.flush() {
            eprintln!("[diagnostic-csv] flush failed: {error}");
        }
    }
}

pub fn start_diagnostic_csv(path: &Path) -> Result<DiagnosticCsv, std::io::Error> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }

    let path = path.to_path_buf();
    let needs_header = metadata(&path).map(|m| m.len() == 0).unwrap_or(true);
    let file = OpenOptions::new().create(true).append(true).open(&path)?;
    let mut writer = BufWriter::new(file);

    if needs_header {
        writeln!(writer, "{}", DiagnosticRow::header())?;
        writer.flush()?;
    }

    Ok(DiagnosticCsv {
        writer: Arc::new(Mutex::new(writer)),
    })
}

struct DiagnosticRow {
    ts_ms: i64,
    event: String,
    direction: String,
    topic: String,
    payload: String,
    connected: bool,
    raw_count: usize,
    last_topic: String,
    last_payload: String,
    actual_az: Option<f64>,
    actual_el: Option<f64>,
    target_az: Option<f64>,
    target_el: Option<f64>,
    actual_az_mech: Option<f64>,
    target_az_mech: Option<f64>,
    az_moving: Option<bool>,
    zen_moving: Option<bool>,
    az_faulted: Option<bool>,
    zen_faulted: Option<bool>,
    imu_roll: Option<f64>,
    imu_pitch: Option<f64>,
    imu_yaw: Option<f64>,
    imu_yaw360: Option<f64>,
    yaw_frame_yaw360: Option<f64>,
    bar_rel_pitch: Option<f64>,
    q: String,
    bar_q: String,
    yaw_q: String,
    bar_rel_q: String,
    imu_valid: Option<bool>,
    imu_startup: Option<bool>,
    rocket_lat: Option<f64>,
    rocket_lon: Option<f64>,
    rocket_alt_m: Option<f64>,
}

impl DiagnosticRow {
    fn header() -> &'static str {
        "host_ts_ms,event,direction,topic,payload,connected,raw_count,last_topic,last_payload,actual_az,actual_el,target_az,target_el,actual_az_mech,target_az_mech,az_moving,zen_moving,az_faulted,zen_faulted,imu_roll,imu_pitch,imu_yaw,imu_yaw360,yaw_frame_yaw360,bar_rel_pitch,q,bar_q,yaw_q,bar_rel_q,imu_valid,imu_startup,rocket_lat,rocket_lon,rocket_alt_m"
    }

    fn from_event(
        event: &str,
        direction: &str,
        topic: &str,
        payload: &str,
        snapshot: &super::telemetry::TelemetrySnapshot,
    ) -> Self {
        let last_raw = snapshot.raw_messages.last();
        Self {
            ts_ms: now_ms(),
            event: event.to_string(),
            direction: direction.to_string(),
            topic: topic.to_string(),
            payload: truncate(payload, 2000),
            connected: snapshot.connected,
            raw_count: snapshot.raw_messages.len(),
            last_topic: last_raw.map(|m| m.topic.clone()).unwrap_or_default(),
            last_payload: last_raw
                .map(|m| truncate(&m.payload, 240))
                .unwrap_or_default(),
            actual_az: number(snapshot.antenna.as_ref(), "actual_az"),
            actual_el: number(snapshot.antenna.as_ref(), "actual_el"),
            target_az: number(snapshot.antenna.as_ref(), "target_az"),
            target_el: number(snapshot.antenna.as_ref(), "target_el"),
            actual_az_mech: number(snapshot.antenna.as_ref(), "actual_az_mech"),
            target_az_mech: number(snapshot.antenna.as_ref(), "target_az_mech"),
            az_moving: boolean(snapshot.antenna.as_ref(), "az_moving"),
            zen_moving: boolean(snapshot.antenna.as_ref(), "zen_moving"),
            az_faulted: boolean(snapshot.antenna.as_ref(), "az_faulted"),
            zen_faulted: boolean(snapshot.antenna.as_ref(), "zen_faulted"),
            imu_roll: number(snapshot.ground_imu.as_ref(), "roll"),
            imu_pitch: number(snapshot.ground_imu.as_ref(), "pitch"),
            imu_yaw: number(snapshot.ground_imu.as_ref(), "yaw"),
            imu_yaw360: number(snapshot.ground_imu.as_ref(), "yaw360"),
            yaw_frame_yaw360: number(snapshot.ground_imu.as_ref(), "yaw_frame_yaw360"),
            bar_rel_pitch: number(snapshot.ground_imu.as_ref(), "bar_rel_pitch"),
            q: array_string(snapshot.ground_imu.as_ref(), "q"),
            bar_q: array_string(snapshot.ground_imu.as_ref(), "bar_q"),
            yaw_q: array_string(snapshot.ground_imu.as_ref(), "yaw_q"),
            bar_rel_q: array_string(snapshot.ground_imu.as_ref(), "bar_rel_q"),
            imu_valid: boolean(snapshot.ground_imu.as_ref(), "valid"),
            imu_startup: boolean(snapshot.ground_imu.as_ref(), "startup"),
            rocket_lat: number(snapshot.latest.as_ref(), "lat")
                .or_else(|| number(snapshot.latest.as_ref(), "latitude")),
            rocket_lon: number(snapshot.latest.as_ref(), "lon")
                .or_else(|| number(snapshot.latest.as_ref(), "longitude")),
            rocket_alt_m: number(snapshot.latest.as_ref(), "alt_m")
                .or_else(|| number(snapshot.latest.as_ref(), "alt_baro_m"))
                .or_else(|| number(snapshot.latest.as_ref(), "alt_gps_m")),
        }
    }

    fn to_csv(&self) -> String {
        [
            self.ts_ms.to_string(),
            csv_escape(&self.event),
            csv_escape(&self.direction),
            csv_escape(&self.topic),
            csv_escape(&self.payload),
            self.connected.to_string(),
            self.raw_count.to_string(),
            csv_escape(&self.last_topic),
            csv_escape(&self.last_payload),
            opt_num(self.actual_az),
            opt_num(self.actual_el),
            opt_num(self.target_az),
            opt_num(self.target_el),
            opt_num(self.actual_az_mech),
            opt_num(self.target_az_mech),
            opt_bool(self.az_moving),
            opt_bool(self.zen_moving),
            opt_bool(self.az_faulted),
            opt_bool(self.zen_faulted),
            opt_num(self.imu_roll),
            opt_num(self.imu_pitch),
            opt_num(self.imu_yaw),
            opt_num(self.imu_yaw360),
            opt_num(self.yaw_frame_yaw360),
            opt_num(self.bar_rel_pitch),
            csv_escape(&self.q),
            csv_escape(&self.bar_q),
            csv_escape(&self.yaw_q),
            csv_escape(&self.bar_rel_q),
            opt_bool(self.imu_valid),
            opt_bool(self.imu_startup),
            opt_num(self.rocket_lat),
            opt_num(self.rocket_lon),
            opt_num(self.rocket_alt_m),
        ]
        .join(",")
    }
}

fn number(value: Option<&Value>, key: &str) -> Option<f64> {
    value?.get(key)?.as_f64()
}

fn boolean(value: Option<&Value>, key: &str) -> Option<bool> {
    value?.get(key)?.as_bool()
}

fn array_string(value: Option<&Value>, key: &str) -> String {
    let Some(values) = value.and_then(|v| v.get(key)).and_then(Value::as_array) else {
        return String::new();
    };

    values
        .iter()
        .map(|v| v.as_f64().map(|n| format!("{n:.6}")).unwrap_or_default())
        .collect::<Vec<_>>()
        .join(";")
}

fn opt_num(value: Option<f64>) -> String {
    value.map(|v| format!("{v:.6}")).unwrap_or_default()
}

fn opt_bool(value: Option<bool>) -> String {
    value.map(|v| v.to_string()).unwrap_or_default()
}

fn truncate(value: &str, max_chars: usize) -> String {
    if value.chars().count() <= max_chars {
        return value.to_string();
    }
    value.chars().take(max_chars).collect()
}

fn csv_escape(value: &str) -> String {
    if value.contains(',') || value.contains('"') || value.contains('\n') || value.contains('\r') {
        format!("\"{}\"", value.replace('"', "\"\""))
    } else {
        value.to_string()
    }
}
