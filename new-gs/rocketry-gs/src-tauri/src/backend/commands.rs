use super::mqtt::PublishRequest;
use super::telemetry::{TelemetrySnapshot, TelemetryState};
use super::tile_cache::{TileCache, TileCacheInfo};
use tauri::State;
use tokio::sync::mpsc;

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
