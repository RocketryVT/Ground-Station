mod backend;

use backend::{
    broker::Broker,
    commands,
    gps::GpsManager,
    logger::{start_diagnostic_csv, PacketLogger},
    mqtt,
    telemetry::TelemetryState,
    tile_cache::TileCache,
};
use tauri::{Manager, RunEvent};

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let data_dir = app
                .path()
                .app_data_dir()
                .unwrap_or_else(|_| std::env::current_dir().expect("current dir"));

            let logger = PacketLogger::open(&data_dir.join("telemetry.db"))?;
            let state = TelemetryState::default();

            let csv_name = format!(
                "diagnostics-{}.csv",
                chrono::Local::now().format("%Y%m%d-%H%M%S")
            );
            let csv_path = std::env::current_exe()
                .ok()
                .and_then(|path| path.parent().map(|parent| parent.join(&csv_name)))
                .unwrap_or_else(|| data_dir.join(&csv_name));
            let diagnostic_csv = start_diagnostic_csv(&csv_path)?;
            state.add_log_line(format!("[diagnostic-csv] writing {}", csv_path.display()));
            diagnostic_csv.log_event(
                "app_start",
                "event",
                "diagnostic-csv",
                &csv_path.display().to_string(),
                &state,
            );

            let tile_cache_dir = std::env::current_exe()
                .ok()
                .and_then(|path| path.parent().map(|parent| parent.join("cesium-tile-cache")))
                .unwrap_or_else(|| data_dir.join("cesium-tile-cache"));
            let tile_cache = {
                let s = state.clone();
                TileCache::start(tile_cache_dir, move |msg| {
                    s.add_log_line(msg);
                })?
            };

            // Start Mosquitto before opening the MQTT connection.
            // Writes mosquitto.conf into data_dir so it listens on 0.0.0.0:1883.
            // rumqttc reconnects every 2 s, so a brief startup delay is fine.
            let broker = {
                let s = state.clone();
                Broker::start(&data_dir, move |msg| {
                    s.add_log_line(msg);
                })
            };

            mqtt::start(
                app.handle().clone(),
                state.clone(),
                logger.clone(),
                diagnostic_csv.clone(),
            );

            app.manage(state);
            app.manage(logger);
            app.manage(diagnostic_csv);
            app.manage(tile_cache);
            app.manage(broker);
            app.manage(GpsManager::default());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_telemetry_snapshot,
            commands::publish_mqtt,
            commands::clear_flight,
            commands::clear_debug,
            commands::clear_raw_sensors,
            commands::clear_ahrs_history,
            commands::get_tile_cache_info,
            commands::save_mag_cal,
            commands::load_mag_cal,
            commands::gps_list_ports,
            commands::gps_connect,
            commands::gps_disconnect,
        ])
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|app, event| {
            if let RunEvent::Exit = event {
                app.state::<GpsManager>().disconnect(app);
                app.state::<Broker>().stop();
                app.state::<TileCache>().stop();
            }
        });
}
