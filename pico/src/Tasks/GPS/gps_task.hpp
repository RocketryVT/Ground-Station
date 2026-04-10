#pragma once

// Spawns the Starlink GPS polling task.
// Polls http://192.168.100.1/api/v1/status every STARLINK_POLL_INTERVAL_MS,
// extracts latitude/longitude/altitudeMeters from the JSON response, and
// writes the result to g_gs_location_q via xQueueOverwrite().
void gps_task_init();
