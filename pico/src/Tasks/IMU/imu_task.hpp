#pragma once

// -- IMU Task ------------------------------------------------------------------
// Reads the ISM330DLC accelerometer/gyroscope (I2C0 @ 0x6A) at 100 Hz.
// Writes IcmMsg to g_icm_q (depth-1 overwrite queue).
// fusion_task consumes g_icm_q to drive the AHRS.

void imu_task_init();
