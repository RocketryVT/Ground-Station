//! USB u-blox GPS source for the antenna tracker.
//!
//! Reads UBX `NAV-PVT` from a user-selected serial port (the `ublox` crate),
//! and republishes each valid fix to the `gs/location` MQTT topic — the same
//! topic the tracker Pico already subscribes to for its own position. Status
//! (fix/sats/lat/lon/alt) is emitted to the UI on `gps://status`.

use super::mqtt::PublishRequest;
use super::telemetry::{TelemetryState, UiEvent, EVENT_NAME};
use serde::Serialize;
use serialport::SerialPortType;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};
use tokio::sync::mpsc;
use ublox::{proto23::PacketRef, GnssFixType, Parser, UbxPacket};

pub const GPS_STATUS_EVENT: &str = "gps://status";

#[derive(Clone, Serialize)]
pub struct SerialPortInfo {
    pub name: String,
    pub kind: String, // "usb" | "bluetooth" | "pci" | "unknown"
    pub product: Option<String>,
    pub manufacturer: Option<String>,
    pub usb: bool,
}

#[derive(Clone, Serialize, Default)]
pub struct GpsStatus {
    pub connected: bool,
    pub port: Option<String>,
    pub fix: String,
    pub satellites: u8,
    pub lat: Option<f64>,
    pub lon: Option<f64>,
    pub alt_m: Option<f64>,
    pub last_fix_ms: Option<i64>,
    pub error: Option<String>,
}

/// Enumerate the serial ports, flagging which look like USB devices.
pub fn list_ports() -> Result<Vec<SerialPortInfo>, String> {
    let mut out: Vec<SerialPortInfo> = serialport::available_ports()
        .map_err(|e| e.to_string())?
        .into_iter()
        .map(|p| {
            let (kind, product, manufacturer, usb) = match &p.port_type {
                SerialPortType::UsbPort(info) => {
                    ("usb", info.product.clone(), info.manufacturer.clone(), true)
                }
                SerialPortType::BluetoothPort => ("bluetooth", None, None, false),
                SerialPortType::PciPort => ("pci", None, None, false),
                SerialPortType::Unknown => ("unknown", None, None, false),
            };
            SerialPortInfo {
                name: p.port_name,
                kind: kind.to_string(),
                product,
                manufacturer,
                usb,
            }
        })
        .collect();

    // The IOKit/udev enumeration above can miss USB CDC modems that a plain
    // `/dev` glob still finds (this is why pyserial/PyGPSClient see them when we
    // don't). Supplement with the call-out / CDC device nodes, de-duped by path.
    #[cfg(unix)]
    if let Ok(entries) = std::fs::read_dir("/dev") {
        for entry in entries.flatten() {
            let file = entry.file_name().to_string_lossy().to_string();
            // macOS call-out devices and Linux USB CDC nodes; skip macOS
            // `tty.*` dial-in nodes (their open() blocks on carrier-detect).
            let is_serial = file.starts_with("cu.")
                || file.starts_with("ttyUSB")
                || file.starts_with("ttyACM");
            if !is_serial {
                continue;
            }
            let path = format!("/dev/{file}");
            if out.iter().any(|p| p.name == path) {
                continue;
            }
            let usb = file.contains("usb") || file.starts_with("ttyACM");
            out.push(SerialPortInfo {
                name: path,
                kind: if usb { "usb" } else { "unknown" }.to_string(),
                product: None,
                manufacturer: None,
                usb,
            });
        }
    }

    out.sort_by(|a, b| b.usb.cmp(&a.usb).then(a.name.cmp(&b.name)));
    Ok(out)
}

/// Owns the reader thread for the currently selected port (if any).
#[derive(Default)]
pub struct GpsManager {
    inner: Mutex<Option<Running>>,
}

struct Running {
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl GpsManager {
    pub fn connect(
        &self,
        app: AppHandle,
        state: TelemetryState,
        publish_tx: mpsc::Sender<PublishRequest>,
        port_name: String,
        baud: u32,
    ) -> Result<(), String> {
        // Drop any existing connection first.
        self.disconnect(&app);

        log(&app, &state, format!("[gps] opening {port_name} @ {baud}"));
        let mut port = match serialport::new(&port_name, baud)
            .timeout(Duration::from_millis(250))
            .open()
        {
            Ok(port) => port,
            Err(e) => {
                let msg = format!("{port_name}: {e}");
                log(&app, &state, format!("[gps] open failed — {msg}"));
                emit_status(
                    &app,
                    &GpsStatus {
                        error: Some(msg.clone()),
                        ..Default::default()
                    },
                );
                return Err(msg);
            }
        };

        // Configure the receiver via CFG-VALSET, RAM layer only (nothing is
        // written to BBR/flash, so a power cycle restores the saved config).
        //
        // The same sequence works on the F9P, M9N and M10. Each frame is sent
        // independently so a NAK on one doesn't drop the others — this also
        // gives the constellations a real priority order: the receiver caps the
        // number of concurrent *major* GNSS (4 on all three of these modules),
        // ACK/NAK-ing each enable against the running total, so the earliest
        // (highest-priority) ones win and a later one over the cap is rejected
        // on its own. QZSS + SBAS are GPS augmentation and don't count to the
        // cap (QZSS must track GPS's enable state, so it ships in the GPS frame).
        for frame in [
            // NAV-PVT output on USB and UART1 (covers direct-USB and USB↔UART).
            cfg_valset(&[
                (0x2091_0009, 1), // CFG-MSGOUT-UBX_NAV_PVT_USB
                (0x2091_0007, 1), // CFG-MSGOUT-UBX_NAV_PVT_UART1
            ]),
            // Priority 1 — US: GPS (+ QZSS + SBAS augmentation).
            cfg_valset(&[
                (0x1031_001f, 1), // CFG-SIGNAL-GPS_ENA
                (0x1031_0024, 1), // CFG-SIGNAL-QZSS_ENA
                (0x1031_0020, 1), // CFG-SIGNAL-SBAS_ENA
            ]),
            // Priority 2 — EU: Galileo.
            cfg_valset(&[(0x1031_0021, 1)]), // CFG-SIGNAL-GAL_ENA
            // Then the rest, each on its own frame so a concurrency cap drops
            // only the lowest-priority extras, never GPS/Galileo.
            cfg_valset(&[(0x1031_0022, 1)]), // CFG-SIGNAL-BDS_ENA  (BeiDou)
            cfg_valset(&[(0x1031_0025, 1)]), // CFG-SIGNAL-GLO_ENA  (GLONASS)
            // Stationary dynamic model (the tracker doesn't move).
            cfg_valset(&[(0x2011_0021, 2)]), // CFG-NAVSPG-DYNMODEL = STATIONARY
            // Base / survey-in mode (F9P only; NAK'd on M9N/M10).
            cfg_valset(&[
                (0x2003_0001, 1),     // CFG-TMODE-MODE = SURVEY_IN
                (0x4003_0010, 60),    // CFG-TMODE-SVIN_MIN_DUR = 60 s
                (0x4003_0011, 50000), // CFG-TMODE-SVIN_ACC_LIMIT = 5 m (0.1 mm units)
            ]),
        ] {
            let _ = port.write_all(&frame);
        }
        let _ = port.flush();
        log(&app, &state, format!("[gps] {port_name} open — configured NAV-PVT, waiting for fix"));

        let stop = Arc::new(AtomicBool::new(false));
        let stop_thread = stop.clone();
        let port_label = port_name.clone();

        let handle = std::thread::Builder::new()
            .name("gps-reader".into())
            .spawn(move || {
                read_loop(app, state, publish_tx, port, port_label, stop_thread);
            })
            .map_err(|e| e.to_string())?;

        *self.inner.lock().unwrap() = Some(Running {
            stop,
            handle: Some(handle),
        });
        Ok(())
    }

    pub fn disconnect(&self, app: &AppHandle) {
        let running = self.inner.lock().unwrap().take();
        if let Some(mut running) = running {
            running.stop.store(true, Ordering::Relaxed);
            if let Some(handle) = running.handle.take() {
                let _ = handle.join();
            }
            emit_status(app, &GpsStatus::default());
        }
    }
}

fn read_loop(
    app: AppHandle,
    state: TelemetryState,
    publish_tx: mpsc::Sender<PublishRequest>,
    mut port: Box<dyn serialport::SerialPort>,
    port_label: String,
    stop: Arc<AtomicBool>,
) {
    let mut parser = Parser::default_proto();
    let mut buf = [0u8; 1024];
    let mut status = GpsStatus {
        connected: true,
        port: Some(port_label.clone()),
        fix: "no-fix".to_string(),
        ..Default::default()
    };
    emit_status(&app, &status);

    let mut total_bytes: u64 = 0;
    let mut nav_pvt_seen = false;
    let mut had_fix = false;
    let mut last_note = Instant::now();

    while !stop.load(Ordering::Relaxed) {
        let n = match port.read(&mut buf) {
            Ok(0) => continue,
            Ok(n) => n,
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                // Nothing arriving at all is a strong hint the wrong port/baud
                // was chosen — surface it once after a few seconds of silence.
                if total_bytes == 0 && last_note.elapsed() > Duration::from_secs(5) {
                    log(&app, &state, format!(
                        "[gps] {port_label}: no bytes after 5 s — check the port/baud"
                    ));
                    last_note = Instant::now();
                }
                continue;
            }
            Err(e) => {
                status.connected = false;
                status.error = Some(format!("{port_label}: {e}"));
                log(&app, &state, format!("[gps] read error — {port_label}: {e}"));
                emit_status(&app, &status);
                return;
            }
        };
        total_bytes += n as u64;

        let mut it = parser.consume_ubx(&buf[..n]);
        while let Some(packet) = it.next() {
            if let Ok(UbxPacket::Proto23(PacketRef::NavPvt(pvt))) = packet {
                if !nav_pvt_seen {
                    nav_pvt_seen = true;
                    log(&app, &state, format!("[gps] {port_label}: NAV-PVT stream up"));
                }
                let fix = pvt.fix_type();
                // NavPvtFlags::GPS_FIX_OK is bit 0.
                let valid = matches!(fix, GnssFixType::Fix2D | GnssFixType::Fix3D)
                    && (pvt.flags().bits() & 0x01) != 0;

                status.fix = fix_label(fix).to_string();
                status.satellites = pvt.num_satellites();

                if valid {
                    let lat = pvt.latitude();
                    let lon = pvt.longitude();
                    let alt_m = pvt.height_msl();
                    status.lat = Some(lat);
                    status.lon = Some(lon);
                    status.alt_m = Some(alt_m);
                    status.last_fix_ms = Some(chrono::Utc::now().timestamp_millis());

                    if !had_fix {
                        had_fix = true;
                        log(&app, &state, format!(
                            "[gps] fix {} — {} sats @ {lat:.6},{lon:.6} -> gs/location",
                            status.fix, status.satellites
                        ));
                    }

                    // Hand the fix to the tracker via the existing publish path;
                    // the codec encodes gs/location JSON -> LocationCommand proto.
                    let payload =
                        format!("{{\"lat\":{lat},\"lon\":{lon},\"alt_m\":{alt_m}}}");
                    let _ = publish_tx.blocking_send(PublishRequest {
                        topic: super::codec::GS_LOCATION.to_string(),
                        payload,
                    });
                }
                emit_status(&app, &status);
            }
        }

        // Receiving bytes but no UBX -> the device is almost certainly emitting
        // NMEA only (the NAV-PVT enable didn't take). Note it once.
        if !nav_pvt_seen && total_bytes > 0 && last_note.elapsed() > Duration::from_secs(5) {
            log(&app, &state, format!(
                "[gps] {port_label}: {total_bytes} bytes but no UBX NAV-PVT yet \
                 (receiver may be NMEA-only / config NAK'd)"
            ));
            last_note = Instant::now();
        }
    }
}

fn log(app: &AppHandle, state: &TelemetryState, msg: impl Into<String>) {
    let line = state.add_log_line(msg.into());
    let _ = app.emit(EVENT_NAME, &UiEvent::LogLine { line });
}

fn fix_label(fix: GnssFixType) -> &'static str {
    match fix {
        GnssFixType::NoFix => "no-fix",
        GnssFixType::DeadReckoningOnly => "dead-reckoning",
        GnssFixType::Fix2D => "2d",
        GnssFixType::Fix3D => "3d",
        GnssFixType::GPSPlusDeadReckoning => "gps+dr",
        GnssFixType::TimeOnlyFix => "time-only",
        _ => "unknown",
    }
}

fn emit_status(app: &AppHandle, status: &GpsStatus) {
    let _ = app.emit(GPS_STATUS_EVENT, status);
}

// -- Minimal UBX frame builders ------------------------------------------------

/// Wrap a class/id/payload in a UBX frame with the Fletcher-8 checksum.
fn ubx_frame(class: u8, id: u8, payload: &[u8]) -> Vec<u8> {
    let len = payload.len() as u16;
    let mut frame = Vec::with_capacity(payload.len() + 8);
    frame.extend_from_slice(&[0xB5, 0x62, class, id, len as u8, (len >> 8) as u8]);
    frame.extend_from_slice(payload);
    let (mut ck_a, mut ck_b) = (0u8, 0u8);
    for &byte in &frame[2..] {
        ck_a = ck_a.wrapping_add(byte);
        ck_b = ck_b.wrapping_add(ck_a);
    }
    frame.push(ck_a);
    frame.push(ck_b);
    frame
}

/// CFG-VALSET (RAM layer only) setting one or more configuration keys. The
/// value width is derived from the key's storage-size field (bits 30:28).
fn cfg_valset(pairs: &[(u32, u64)]) -> Vec<u8> {
    let mut payload = vec![0x00, 0x01, 0x00, 0x00]; // version, layers=RAM, reserved
    for &(key, value) in pairs {
        payload.extend_from_slice(&key.to_le_bytes());
        let nbytes = match (key >> 28) & 0x7 {
            0x1 | 0x2 => 1, // bit / one byte
            0x3 => 2,
            0x4 => 4,
            0x5 => 8,
            _ => 1,
        };
        payload.extend_from_slice(&value.to_le_bytes()[..nbytes]);
    }
    ubx_frame(0x06, 0x8A, &payload)
}
