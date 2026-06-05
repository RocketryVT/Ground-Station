use rumqttd::{Broker as RumqttBroker, Config, ConnectionSettings, RouterConfig, ServerSettings};
use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

#[derive(Clone, Default)]
pub struct Broker {
    handle: Arc<Mutex<Option<JoinHandle<()>>>>,
}

impl Broker {
    /// Spawn an embedded rumqttd broker configured for LAN clients on 0.0.0.0:1883.
    pub fn start(
        _data_dir: &std::path::Path,
        log: impl Fn(String) + Clone + Send + 'static,
    ) -> Self {
        let broker = Self::default();
        let thread_log = log.clone();
        let handle = thread::Builder::new()
            .name("mqtt-broker".to_string())
            .spawn(move || {
                let mut embedded = RumqttBroker::new(config());
                thread_log("[broker] starting embedded rumqttd on 0.0.0.0:1883".to_string());

                if let Err(error) = embedded.start() {
                    thread_log(format!("[broker] embedded rumqttd stopped: {error}"));
                }
            });

        match handle {
            Ok(handle) => {
                *broker.handle.lock().unwrap() = Some(handle);
            }
            Err(error) => {
                log(format!(
                    "[broker] could not start embedded rumqttd: {error}"
                ));
            }
        }

        broker
    }

    /// rumqttd's public embedded API does not expose graceful shutdown.
    /// The broker thread is terminated with the Tauri process.
    pub fn stop(&self) {
        if self.handle.lock().unwrap().is_some() {
            eprintln!("[broker] embedded rumqttd will stop with the app process");
        }
    }
}

fn config() -> Config {
    let listen = SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), 1883);
    let connections = ConnectionSettings {
        connection_timeout_ms: 10_000,
        max_payload_size: 1024 * 1024,
        max_inflight_count: 100,
        auth: None,
        external_auth: None,
        dynamic_filters: true,
    };

    let server = ServerSettings {
        name: "rocketry-gs-mqtt-v4".to_string(),
        listen,
        tls: None,
        next_connection_delay_ms: 1,
        connections,
    };

    let mut v4 = HashMap::new();
    v4.insert("lan".to_string(), server);

    Config {
        id: 0,
        router: RouterConfig {
            max_connections: 128,
            max_outgoing_packet_count: 10_000,
            max_segment_size: 1024 * 1024,
            max_segment_count: 10,
            custom_segment: None,
            initialized_filters: None,
            shared_subscriptions_strategy: Default::default(),
        },
        v4: Some(v4),
        v5: None,
        ws: None,
        cluster: None,
        console: None,
        bridge: None,
        prometheus: None,
        metrics: None,
    }
}
