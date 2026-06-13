#include "rtc.hpp"
#include "shared.hpp"

#include "pico/aon_timer.h"        // hardware-backed wall clock (powman on RP2350,
                                   // legacy RTC on RP2040) — portable across boards
#include "pico/util/datetime.h"    // ms_to_timespec / timespec_to_ms (unguarded helpers)
#include "pico/time.h"

#include <time.h>

// -- Discipline state ----------------------------------------------------------
// The disciplined wall clock lives in the always-on timer; these track which
// source owns it and the warm-up/resync bookkeeping.  GPS state is written only
// by gps_task, NTP state only by ntp_task, so no locking is needed beyond the
// always-on timer's own atomicity.
static bool      s_synced          = false;     // any source has set the clock
static RtcSource s_source          = RtcSource::None;

// GPS path
static bool      s_gps_synced      = false;     // GPS warm-up complete
static uint32_t  s_warm_count      = 0;         // samples gathered during warm-up
static int64_t   s_warm_sum_ms     = 0;         // sum of warm-up offsets
static uint64_t  s_last_gps_seen_us = 0;        // last *valid* GPS fix (liveness)
static uint64_t  s_last_gps_disc_us = 0;        // last GPS discipline (resync gate)

// Days since the Unix epoch (1970-01-01) for a civil date.
// Howard Hinnant's algorithm; kept self-contained (no SDK dependency) so the
// epoch math ports cleanly to non-Pico targets.
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Push a Unix-epoch-ms value into the always-on timer, starting it on first use.
static void apply_epoch_ms(uint64_t utc_ms)
{
    struct timespec ts;
    ms_to_timespec(utc_ms, &ts);
    if (aon_timer_is_running())
        aon_timer_set_time(&ts);
    else
        aon_timer_start(&ts);
    s_synced = true;
}

// Reconstruct "now" from a UTC/local-clock offset and commit it to the clock.
// `offset_ms` is (true UTC ms) - (local monotonic ms) at the sample instant;
// it is time-invariant, so adding the current monotonic clock yields UTC now.
static void commit_offset(int64_t offset_ms, RtcSource src)
{
    const uint64_t now_utc =
        static_cast<uint64_t>(offset_ms + static_cast<int64_t>(time_us_64() / 1000ULL));
    apply_epoch_ms(now_utc);
    s_source = src;
}

// True when GPS is actively producing fixes and should keep NTP locked out.
static bool gps_is_live(uint64_t now_us)
{
    return s_gps_synced &&
           (now_us - s_last_gps_seen_us)
               < static_cast<uint64_t>(RTC_GPS_HOLDOFF_MS) * 1000ULL;
}

void rtc_submit_gps_time(uint16_t year, uint8_t month, uint8_t day,
                         uint32_t ms_of_day, uint64_t capture_us)
{
    // Reject samples without a resolved UTC date (validDate clear => year == 0).
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31)
        return;

    const uint64_t utc_ms =
        static_cast<uint64_t>(days_from_civil(year, month, day)) * 86400000ULL
        + ms_of_day;
    const int64_t offset_ms = static_cast<int64_t>(utc_ms)
                            - static_cast<int64_t>(capture_us / 1000ULL);

    s_last_gps_seen_us = capture_us;            // liveness — every valid fix

    // -- Warm-up: average the first RTC_WARMUP_SAMPLES offsets ------------------
    if (!s_gps_synced) {
        s_warm_sum_ms += offset_ms;
        if (++s_warm_count >= RTC_WARMUP_SAMPLES) {
            const RtcSource prev = s_source;
            commit_offset(s_warm_sum_ms / s_warm_count, RtcSource::Gps);
            s_gps_synced       = true;
            s_last_gps_disc_us = capture_us;
            log_print("[rtc] GPS sync after %u samples: utc=%llu ms%s\n",
                      static_cast<unsigned>(s_warm_count),
                      static_cast<unsigned long long>(rtc_now_ms()),
                      prev == RtcSource::Ntp ? " (took over from NTP)" : "");
        }
        return;
    }

    // -- Steady state: re-discipline at most once per resync interval -----------
    if (capture_us - s_last_gps_disc_us
            < static_cast<uint64_t>(RTC_RESYNC_INTERVAL_MS) * 1000ULL)
        return;
    s_last_gps_disc_us = capture_us;

    const int64_t drift_ms = offset_ms
        + static_cast<int64_t>(time_us_64() / 1000ULL)
        - static_cast<int64_t>(rtc_now_ms());
    commit_offset(offset_ms, RtcSource::Gps);
    log_print("[rtc] GPS resync: drift %lld ms  utc=%llu ms\n",
              static_cast<long long>(drift_ms),
              static_cast<unsigned long long>(rtc_now_ms()));
}

void rtc_submit_ntp_time(uint64_t utc_ms, uint64_t capture_us)
{
    // GPS preferred: ignore NTP while GPS is live.  NTP is still accepted during
    // GPS warm-up (s_gps_synced == false) so the clock is set as early as possible.
    if (gps_is_live(capture_us))
        return;

    const int64_t offset_ms = static_cast<int64_t>(utc_ms)
                            - static_cast<int64_t>(capture_us / 1000ULL);
    const RtcSource prev = s_source;
    const int64_t drift_ms = s_synced
        ? offset_ms + static_cast<int64_t>(time_us_64() / 1000ULL)
              - static_cast<int64_t>(rtc_now_ms())
        : 0;

    commit_offset(offset_ms, RtcSource::Ntp);

    if (prev != RtcSource::Ntp)
        log_print("[rtc] NTP %s: utc=%llu ms\n",
                  prev == RtcSource::Gps ? "took over (GPS stale)" : "sync",
                  static_cast<unsigned long long>(rtc_now_ms()));
    else
        log_print("[rtc] NTP resync: drift %lld ms  utc=%llu ms\n",
                  static_cast<long long>(drift_ms),
                  static_cast<unsigned long long>(rtc_now_ms()));
}

uint64_t rtc_now_ms()
{
    if (!s_synced) return 0;
    struct timespec ts;
    if (!aon_timer_get_time(&ts)) return 0;
    return timespec_to_ms(&ts);
}

bool rtc_is_synced() { return s_synced; }

RtcSource rtc_source() { return s_source; }
