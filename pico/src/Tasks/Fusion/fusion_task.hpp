#pragma once

// -- Fusion Task ---------------------------------------------------------------
// Runs the Fusion AHRS (Madgwick-derived, x-io Technologies) at 100 Hz.
// Peeks g_icm_q, g_mag_q, g_baro_q for latest sensor data.
// Writes ImuMsg to g_imu_q (depth-1 overwrite).
// Publishes orientation to MQTT at 1 Hz.

void  fusion_task_init();
void  fusion_set_declination( float deg );
float fusion_get_declination();
void  fusion_adjust_heading_offset( float delta_deg );
float fusion_get_heading_offset();

// Apply hard/soft-iron mag calibration at runtime. yaw=true targets the yaw
// platform LIS3MDL, false the bar/zenith LIS3MDL. hard_iron is [hx,hy,hz];
// soft_iron is a 3x3 row-major matrix (9 elements).
void  fusion_set_mag_calibration( bool yaw, const float hard_iron[3], const float soft_iron[9] );
