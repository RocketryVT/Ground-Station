#pragma once

// Spawns the WiFi management task.
// The task connects to the AP defined by WIFI_SSID / WIFI_PASSWORD (CMake),
// sets EVT_WIFI_CONNECTED in g_net_events on success, and clears it if the
// link drops (triggering automatic reconnection).
void wifi_task_init();
