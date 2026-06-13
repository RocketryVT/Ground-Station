#pragma once

// -- NTP fallback time source --------------------------------------------------
// The RP2350 has no RP2040-style RTC peripheral.  Wall-clock time is kept by the
// unified RTC service (Tasks/RTC/rtc.hpp), which is GPS-preferred with NTP as a
// fallback.  This task runs SNTP against a public server and forwards each
// response to rtc_submit_ntp_time(); the RTC decides whether to apply it (it is
// ignored while GPS is live).
//
// Read wall-clock time via rtc_now_ms() / rtc_is_synced(), not from here.

// Spawn the NTP sync task.  Call after wifi_task_init().
void ntp_task_init();
