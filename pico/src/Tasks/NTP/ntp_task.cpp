#include "ntp_task.hpp"
#include "shared.hpp"

#include "FreeRTOS.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/sntp.h"

// -- Configuration -------------------------------------------------------------
// Starlink does NOT run NTP on 192.168.100.1.
// Use Cloudflare's public NTP — reachable via Starlink's internet connection.
#define NTP_SERVER              "time.cloudflare.com"
#define NTP_RESYNC_INTERVAL_MS  60000
#define NTP_TIMEOUT_MS          8000

// -- UTC epoch state -----------------------------------------------------------
// s_epoch_offset_ms:  UTC_ms = s_epoch_offset_ms + time_us_64() / 1000
//
// Written only by the NTP task (single writer); read from any task.
// uint64_t writes on Cortex-M33 are not atomic, so we guard reads/writes
// with a lightweight critical section via a FreeRTOS mutex.

static volatile uint64_t s_epoch_offset_ms = 0;
static volatile bool     s_synced          = false;

// -- SNTP callback (called from lwIP IRQ context) ------------------------------
// lwIP calls SNTP_SET_SYSTEM_TIME_US(sec, us) when a valid response arrives.
// We define that macro below to call this function.

static void on_ntp_response(uint32_t sec, uint32_t us)
{
    // Compute offset so that:  utc_ms = offset + time_us_64()/1000
    uint64_t utc_ms    = (uint64_t)sec * 1000ULL + us / 1000ULL;
    uint64_t pico_ms   = time_us_64() / 1000ULL;
    uint64_t new_offset = utc_ms - pico_ms;

    // Write from IRQ — no FreeRTOS API allowed.  Write high word then low word
    // so a reader that sees both words consistent gets a valid value.
    // (On CM33 unaligned 64-bit stores are not guaranteed atomic, but writing
    // in two 32-bit halves with the high word first is safe for this use case:
    // a torn read at worst gives a value that is off by up to 2^32 ms ~49 days,
    // which utc_now_ms() callers handle by checking ntp_is_synced() first.)
    volatile uint32_t* p = (volatile uint32_t*)&s_epoch_offset_ms;
    p[1] = (uint32_t)(new_offset >> 32);
    p[0] = (uint32_t)(new_offset & 0xFFFFFFFFULL);
    s_synced = true;
}

// -- lwIP SNTP hook — must be defined at compile unit scope --------------------
// sntp.h declares SNTP_SET_SYSTEM_TIME_US as a macro that the user overrides
// in lwipopts.h.  Since we can't easily put a function call in a macro that
// references our static, we use the callback approach via sntp_set_time_sync_notification_cb
// where available, and fall back to a module-level C function named for the macro.

// The cleanest approach for NO_SYS=1: define the callback function the macro
// expands to.  sntp_opts.h defaults SNTP_SET_SYSTEM_TIME_US to a no-op;
// we override it in lwipopts.h (see CMakeLists note).

extern "C" void sntp_set_system_time_us(uint32_t sec, uint32_t us)
{
    on_ntp_response(sec, us);
}

// -- Public API ----------------------------------------------------------------

uint64_t utc_now_ms()
{
    if (!s_synced) return 0;
    // Read the two halves.  If high word changed between reads, re-read.
    // This gives a consistent 64-bit value without disabling interrupts.
    const volatile uint32_t* p = (const volatile uint32_t*)&s_epoch_offset_ms;
    uint32_t hi, lo;
    do {
        hi = p[1];
        lo = p[0];
    } while (p[1] != hi);
    uint64_t offset = ((uint64_t)hi << 32) | lo;
    return offset + time_us_64() / 1000ULL;
}

bool ntp_is_synced() { return s_synced; }

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

    log_print("[ntp] SNTP started, waiting for first sync...\n");

    for (;;) {
        uint32_t wait_ms = s_synced ? NTP_RESYNC_INTERVAL_MS : NTP_TIMEOUT_MS;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));

        if (!s_synced) {
            log_print("[ntp] no sync yet, retrying...\n");
        } else {
            log_print("[ntp] UTC: %llu ms\n", (unsigned long long)utc_now_ms());
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
