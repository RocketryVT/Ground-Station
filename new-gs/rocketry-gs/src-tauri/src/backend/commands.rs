use super::mqtt::PublishRequest;
use super::telemetry::{TelemetrySnapshot, TelemetryState};
use super::tile_cache::{TileCache, TileCacheInfo};
use std::path::PathBuf;
use tauri::State;
use tokio::sync::mpsc;

/// Path of the magnetometer calibration store, kept next to the running binary
/// so it survives restarts and is re-applied to the Pico on each connect.
fn mag_cal_path() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let dir = exe
        .parent()
        .ok_or_else(|| "executable has no parent directory".to_string())?;
    Ok(dir.join("mag_cal.json"))
}

#[tauri::command]
pub fn save_mag_cal(payload: String) -> Result<(), String> {
    let path = mag_cal_path()?;
    std::fs::write(&path, payload).map_err(|e| format!("{}: {e}", path.display()))
}

#[tauri::command]
pub fn load_mag_cal() -> Result<Option<String>, String> {
    let path = mag_cal_path()?;
    match std::fs::read_to_string(&path) {
        Ok(contents) => Ok(Some(contents)),
        Err(ref e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("{}: {e}", path.display())),
    }
}

#[tauri::command]
pub fn get_telemetry_snapshot(state: State<'_, TelemetryState>) -> TelemetrySnapshot {
    state.snapshot()
}

#[tauri::command]
pub async fn publish_mqtt(
    tx: State<'_, mpsc::Sender<PublishRequest>>,
    topic: String,
    payload: String,
) -> Result<(), String> {
    tx.send(PublishRequest { topic, payload })
        .await
        .map_err(|error| error.to_string())
}

#[tauri::command]
pub fn clear_flight(state: State<'_, TelemetryState>) {
    state.clear_flight();
}

#[tauri::command]
pub fn clear_debug(state: State<'_, TelemetryState>) {
    state.clear_debug();
}

#[tauri::command]
pub fn clear_raw_sensors(state: State<'_, TelemetryState>) {
    state.clear_raw_sensors();
}

#[tauri::command]
pub fn clear_ahrs_history(state: State<'_, TelemetryState>) {
    state.clear_ahrs_history();
}

#[tauri::command]
pub fn get_tile_cache_info(tile_cache: State<'_, TileCache>) -> TileCacheInfo {
    tile_cache.info()
}
