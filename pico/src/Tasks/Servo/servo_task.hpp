#pragma once

// Spawns the servo tracking task.
// Reads g_gs_location_q (ground station GPS) and g_rocket_location_q
// (rocket GPS, populated by LoRa task once the data format is defined).
// Computes azimuth and elevation via GroundStationMath and drives the
// two hobby servos on GPIO SERVO_AZIMUTH and SERVO_ZENITH via PWM.
void servo_task_init();
