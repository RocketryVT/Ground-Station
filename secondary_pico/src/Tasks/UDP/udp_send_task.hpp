#pragma once

// UDP sender task.
// Drains g_udp_queue and sends each UdpFrame to the primary Pico
// at PRIMARY_PICO_IP:INTER_PICO_PORT via lwIP raw UDP.
// Waits for EVT_WIFI_CONNECTED before opening the socket.
void udp_send_task_init();
