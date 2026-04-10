#pragma once

// -- Fusion Task ---------------------------------------------------------------
// Runs the Fusion AHRS (Madgwick-derived, x-io Technologies) at 100 Hz.
// Peeks g_icm_q, g_mag_q, g_baro_q for latest sensor data.
// Writes ImuMsg to g_imu_q (depth-1 overwrite).
// Publishes orientation to MQTT at 1 Hz.

void fusion_task_init();
