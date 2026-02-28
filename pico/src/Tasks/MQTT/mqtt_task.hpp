#pragma once

// Spawns the MQTT management task.
// Waits for EVT_WIFI_CONNECTED, then connects to MQTT_BROKER_HOST.
// Drains g_mqtt_queue and publishes each message while the broker connection
// is live.  Reconnects automatically if the broker drops.
void mqtt_task_init();

// True while the broker TCP connection is accepted and active.
bool mqtt_is_connected();
