#pragma once

// -- Barometer Task ------------------------------------------------------------
// Reads the MS5611 barometer (I2C0 @ 0x77).
// Alternates D1 (pressure) and D2 (temperature) OSR-4096 conversions each
// 10 ms cycle, producing a calibrated BaroMsg at ~50 Hz.
// Writes BaroMsg to g_baro_q (depth-1 overwrite queue).
// fusion_task peeks g_baro_q for altitude.

void baro_task_init();
