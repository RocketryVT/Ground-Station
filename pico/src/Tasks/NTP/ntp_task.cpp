#include "ntp_task.hpp"
#include "shared.hpp"
#include "Tasks/RTC/rtc.hpp"

#include "FreeRTOS.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/sntp.h"

// -- Configuration -------------------------------------------------------------
// Starlink does NOT run NTP on 192.168.100.1.
// Use Cloudflare's public NTP — reachable via Starlink's internet connection.
#define NTP_SERVER              "time.cloudflare.com"
#define NTP_POLL_INTERVAL_MS    60000
#define NTP_TIMEOUT_MS          8000

// -- SNTP response handoff (lwIP callback -> task) -----------------------------
// SNTP_SET_SYSTEM_TIME_US (see lwipopts.h) calls sntp_set_system_time_us() from
// lwIP context, which on this NO_SYS=1 / SMP build may run on either core and
// must not touch FreeRTOS APIs.  We hand the timestamp to ntp_task via a
// seqlock: the callback captures time_us_64() at the instant of the response and
// the task forwards it to the RTC service (where log_print + aon_timer are safe).
static volatile uint32_t s_seq        = 0;   // even = stable, odd = write in progress
static volatile uint64_t s_ntp_utc_ms = 0;   // Unix-epoch ms from the server
static volatile uint64_t s_ntp_cap_us = 0;   // time_us_64() when it arrived
static volatile bool     s_ntp_have   = false;

extern "C" void sntp_set_system_time_us(uint32_t sec, uint32_t us)
{
    const uint64_t cap_us = time_us_64();
    const uint64_t utc_ms = static_cast<uint64_t>(sec) * 1000ULL + us / 1000ULL;

    s_seq = s_seq + 1;             // -> odd: write in progress
    __sync_synchronize();
    s_ntp_utc_ms = utc_ms;
    s_ntp_cap_us = cap_us;
    __sync_synchronize();
    s_seq = s_seq + 1;             // -> even: stable
    s_ntp_have = true;
}

// Read the latest deposited sample.  Returns false if none has arrived yet.
static bool ntp_read_latest(uint64_t* utc_ms, uint64_t* cap_us)
{
    if (!s_ntp_have) return false;
    uint32_t s1, s2;
    do {
        s1 = s_seq;
        if (s1 & 1u) continue;     // a write is in progress — retry
        __sync_synchronize();
        *utc_ms = s_ntp_utc_ms;
        *cap_us = s_ntp_cap_us;
        __sync_synchronize();
        s2 = s_seq;
    } while (s1 != s2);
    return true;
}

// -- Task ----------------------------------------------------------------------

static void ntp_task(void*)
{
    log_print("[ntp] task started, server: %s\n", NTP_SERVER);

    xEventGroupWaitBits(g_net_events, EVT_WIFI_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, NTP_SERVER);
    sntp_init();
    cyw43_arch_lwip_end();

    log_print("[ntp] SNTP started, waiting for first response...\n");

    uint64_t last_cap_us = 0;

    for (;;) {
        const uint32_t wait_ms = s_ntp_have ? NTP_POLL_INTERVAL_MS : NTP_TIMEOUT_MS;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));

        uint64_t utc_ms, cap_us;
        if (ntp_read_latest(&utc_ms, &cap_us) && cap_us != last_cap_us) {
            // New response since last loop — forward to the RTC (it decides
            // whether to apply it; ignored while GPS is live).
            last_cap_us = cap_us;
            rtc_submit_ntp_time(utc_ms, cap_us);
        } else if (!s_ntp_have) {
            log_print("[ntp] no response yet, retrying...\n");
        }

        EventBits_t bits = xEventGroupGetBits(g_net_events);
        if (!(bits & EVT_WIFI_CONNECTED)) {
            log_print("[ntp] WiFi lost, waiting for reconnect\n");
            xEventGroupWaitBits(g_net_events, EVT_WIFI_CONNECTED,
                                pdFALSE, pdTRUE, portMAX_DELAY);
            cyw43_arch_lwip_begin();
            sntp_stop();
            sntp_setservername(0, NTP_SERVER);
            sntp_init();
            cyw43_arch_lwip_end();
            log_print("[ntp] SNTP restarted after reconnect\n");
        }
    }
}

static StaticTask_t s_ntp_tcb;
static StackType_t  s_ntp_stack[512];

void ntp_task_init()
{
    task_create(ntp_task, "ntp", 512, nullptr, tskIDLE_PRIORITY + 2,
                s_ntp_stack, &s_ntp_tcb);
}
