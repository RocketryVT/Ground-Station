#pragma once

#include <stdint.h>
#include <stdbool.h>

// -- NTP / UTC time service ----------------------------------------------------
// The RP2350 has no hardware RTC.  Instead we keep a software UTC epoch:
//
//   utc_ms = s_epoch_offset_ms + time_us_64() / 1000
//
// s_epoch_offset_ms is set once by the NTP task after the first successful
// SNTP response from Starlink's local NTP server (192.168.100.1).
// It is re-synced every NTP_RESYNC_INTERVAL_MS thereafter.
//
// All tasks that need wall-clock time call utc_now_ms() — it is lock-free
// and safe to call from any FreeRTOS task.

// Spawn the NTP sync task.  Call after wifi_task_init().
void ntp_task_init();

// Return current UTC time as milliseconds since Unix epoch.
// Returns 0 if NTP has never synced successfully.
uint64_t utc_now_ms();

// True once the first NTP sync has completed successfully.
bool ntp_is_synced();
