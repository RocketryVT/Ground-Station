#pragma once

#include <stdint.h>
#include <stdbool.h>

// -- Unified software RTC (GPS-preferred, NTP fallback) ------------------------
// One disciplined wall clock backed by the always-on timer (powman on RP2350,
// legacy RTC on RP2040 — see pico_aon_timer).  Two sources feed it:
//
//   * GPS  (rtc_submit_gps_time)  — high priority.  Warms up over
//     RTC_WARMUP_SAMPLES samples, then re-disciplines every RTC_RESYNC_INTERVAL_MS.
//   * NTP  (rtc_submit_ntp_time)  — fallback.  Accepted only while GPS is NOT
//     live (no synced GPS fix seen within RTC_GPS_HOLDOFF_MS).
//
// Either, both, or neither source may be available at any time:
//   * Both live   -> GPS owns the clock; NTP samples are ignored.
//   * GPS only    -> GPS owns the clock.
//   * NTP only    -> NTP owns the clock (also holds it during GPS warm-up).
//   * Neither     -> clock unset; rtc_now_ms() returns 0, rtc_is_synced() false.
//
// rtc_now_ms()/rtc_is_synced()/rtc_source() are safe to call from any task.
// The submit functions assume a single caller each (gps_task, ntp_task).

#define RTC_WARMUP_SAMPLES      5
#define RTC_RESYNC_INTERVAL_MS  60000
#define RTC_GPS_HOLDOFF_MS      10000   // GPS keeps NTP locked out for this long
                                        // after the last seen fix

enum class RtcSource : uint8_t { None = 0, Ntp = 1, Gps = 2 };

// -- GPS feed -- called by gps_task once per NAV-PVT ---------------------------
//   year/month/day : civil UTC date (year e.g. 2026; 0/invalid is ignored)
//   ms_of_day      : milliseconds since UTC midnight (Coordinate::utc_ms)
//   capture_us     : time_us_64() captured when the fix was read
void rtc_submit_gps_time(uint16_t year, uint8_t month, uint8_t day,
                         uint32_t ms_of_day, uint64_t capture_us);

// -- NTP feed -- called by ntp_task when an SNTP response is available ---------
//   utc_ms     : Unix-epoch milliseconds reported by the server
//   capture_us : time_us_64() captured when the SNTP response arrived
void rtc_submit_ntp_time(uint64_t utc_ms, uint64_t capture_us);

// Current UTC time as milliseconds since the Unix epoch.  0 if never synced.
uint64_t rtc_now_ms();

// True once any source has disciplined the clock at least once.
bool rtc_is_synced();

// Which source most recently disciplined the clock.
RtcSource rtc_source();
