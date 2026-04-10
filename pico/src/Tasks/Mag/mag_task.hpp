#pragma once

// -- Magnetometer Task ---------------------------------------------------------
// Reads the LIS3MDL magnetometer (I2C0 @ 0x1C) at 80 Hz.
// Writes MagMsg to g_mag_q (depth-1 overwrite queue).
// fusion_task peeks g_mag_q each cycle.

void mag_task_init();
