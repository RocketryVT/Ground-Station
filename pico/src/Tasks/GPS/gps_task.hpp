#pragma once

// Spawns the GPS receive/parse task.
// Reads NMEA sentences from UART1 (GPIO 8 TX / GPIO 9 RX at 9600 baud),
// parses them with gps::GPSParser, and pushes a JSON fix to g_mqtt_queue
// under the topic "rocket/gps" at most once per second.
void gps_task_init();
