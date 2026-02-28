#include "usb_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define STATUS_PERIOD_MS  10000   // print status every 10 s

// ── log_print ─────────────────────────────────────────────────────────────────
// Defined here so that this translation unit is the sole caller of printf.
// All other tasks call log_print() instead of printf() directly, avoiding
// stdio contention under FreeRTOS SMP.
//
// Non-blocking: if the queue is full the message is silently dropped rather
// than stalling the calling task.  Increase LOG_QUEUE_DEPTH in shared.hpp if
// drops are observed.
void log_print( const char* fmt, ... )
{
    LogMessage msg;
    va_list args;
    va_start( args, fmt );
    vsnprintf( msg.buf, sizeof( msg.buf ), fmt, args );
    va_end( args );

    // xQueueSend must not be called from ISR — see shared.hpp note.
    xQueueSend( g_log_queue, &msg, 0 );
}

// ── USB console task ──────────────────────────────────────────────────────────
static void usb_task( void* param )
{
    ( void ) param;

    LogMessage msg;
    TickType_t last_status = 0;

    for ( ;; ) {
        // Block up to 100 ms for a new log message, then drain the rest
        if ( xQueueReceive( g_log_queue, &msg, pdMS_TO_TICKS( 100 ) ) == pdTRUE ) {
            printf( "%s", msg.buf );

            // Drain any messages that arrived while we were printing
            while ( xQueueReceive( g_log_queue, &msg, 0 ) == pdTRUE ) {
                printf( "%s", msg.buf );
            }
        }

        // Periodic status line
        TickType_t now = xTaskGetTickCount();
        if ( ( now - last_status ) >= pdMS_TO_TICKS( STATUS_PERIOD_MS ) ) {
            last_status = now;
            printf( "[status] heap=%zu B  wifi=%s  mqtt=%s  log_q=%u/%u  mqtt_q=%u/%u\n",
                    xPortGetFreeHeapSize(),
                    ( xEventGroupGetBits( g_net_events ) & EVT_WIFI_CONNECTED ) ? "up" : "down",
                    mqtt_is_connected() ? "up" : "down",
                    ( unsigned ) uxQueueMessagesWaiting( g_log_queue  ), ( unsigned ) LOG_QUEUE_DEPTH,
                    ( unsigned ) uxQueueMessagesWaiting( g_mqtt_queue ), ( unsigned ) MQTT_QUEUE_DEPTH );
        }
    }
}

static StaticTask_t s_usb_tcb;
static StackType_t  s_usb_stack[ 512 ];

void usb_task_init()
{
    // Lowest priority — status printing should never pre-empt real work
    configASSERT( xTaskCreateStatic( usb_task, "usb", 512,
                                      NULL, tskIDLE_PRIORITY + 1,
                                      s_usb_stack, &s_usb_tcb ) );
}
