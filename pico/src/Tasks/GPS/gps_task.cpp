#include "gps_task.hpp"
#include "shared.hpp"

// ── GPS from Starlink (WiFi / MQTT) ──────────────────────────────────────────
// The previous UART1 NMEA implementation has been removed.  Ground-station
// position now arrives via MQTT subscription on topic "gs/gps":
//
//   Payload (JSON): {"lat":<f>,"lon":<f>,"alt":<f>}
//
// The MQTT task needs to be extended with an lwIP MQTT subscribe call and a
// callback that parses this payload and calls:
//
//   LocationMsg loc = { lat, lon, alt_m };
//   xQueueOverwrite( g_gs_location_q, &loc );
//
// Until that is implemented, the servo controller will track without a
// ground-station reference (relative bearing only).
//
// This task is a placeholder that logs the pending TODO on startup.

static void gps_task( void* )
{
    log_print( "[gps] GPS source: Starlink WiFi (MQTT topic: gs/gps)\n" );
    log_print( "[gps] TODO: add lwIP MQTT subscribe in mqtt_task.cpp\n" );
    log_print( "[gps]       parse {\"lat\":...,\"lon\":...,\"alt\":...}\n" );
    log_print( "[gps]       call xQueueOverwrite(g_gs_location_q, &loc)\n" );

    // Nothing to do until MQTT subscribe is wired up.
    while ( true ) {
        vTaskDelay( portMAX_DELAY );
    }
}

static StaticTask_t s_gps_tcb;
static StackType_t  s_gps_stack[ 256 ];

void gps_task_init()
{
    task_create( gps_task, "gps", 256, nullptr, tskIDLE_PRIORITY + 1,
                  s_gps_stack, &s_gps_tcb );
}
