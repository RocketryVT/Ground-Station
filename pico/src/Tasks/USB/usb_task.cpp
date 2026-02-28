#include "usb_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "pico/stdlib.h"

// ── log_print ─────────────────────────────────────────────────────────────────
// Sole caller of printf() in the whole project.  All other tasks call
// log_print() to keep stdio off the FreeRTOS SMP shared stack.
// Non-blocking: message silently dropped if the queue is full.
void log_print( const char* fmt, ... )
{
    LogMessage msg;
    va_list args;
    va_start( args, fmt );
    vsnprintf( msg.buf, sizeof( msg.buf ), fmt, args );
    va_end( args );
    xQueueSend( g_log_queue, &msg, 0 );
}

// ── Console output flag ───────────────────────────────────────────────────────
// Default true for ground station — all log_print() output is visible without
// needing to type "log on".  Use "log off" to suppress if needed.
static bool s_log_enabled = true;

// ── Commands ──────────────────────────────────────────────────────────────────

static void cmd_help()
{
    printf(
        "Commands:\n"
        "  help           show this list\n"
        "  status         print heap, queue depths, WiFi/MQTT state\n"
        "  log   [on|off] toggle/set task log message output (default: off)\n"
        "  clear          clear terminal screen\n"
    );
}

static void cmd_status()
{
    printf( "heap free  : %u B\n",
            ( unsigned ) xPortGetFreeHeapSize() );
    printf( "wifi       : %s\n",
            ( xEventGroupGetBits( g_net_events ) & EVT_WIFI_CONNECTED ) ? "up" : "down" );
    printf( "mqtt       : %s\n",
            mqtt_is_connected() ? "up" : "down" );
    printf( "log_q      : %u / %u\n",
            ( unsigned ) uxQueueMessagesWaiting( g_log_queue  ), ( unsigned ) LOG_QUEUE_DEPTH );
    printf( "mqtt_q     : %u / %u\n",
            ( unsigned ) uxQueueMessagesWaiting( g_mqtt_queue ), ( unsigned ) MQTT_QUEUE_DEPTH );
    printf( "log output : %s\n", s_log_enabled ? "on" : "off" );
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static void dispatch( const char* line, size_t len )
{
    if ( len == 0 ) return;

    if ( strncmp( line, "help",   4 ) == 0 ) { cmd_help();   return; }
    if ( strncmp( line, "status", 6 ) == 0 ) { cmd_status(); return; }

    if ( strncmp( line, "clear",  5 ) == 0 ) {
        printf( "\x1b[2J\x1b[H" );
        stdio_flush();
        return;
    }

    if ( strncmp( line, "log", 3 ) == 0 ) {
        const char* arg = line + 3;
        while ( *arg == ' ' ) arg++;
        if      ( strncmp( arg, "on",  2 ) == 0 ) s_log_enabled = true;
        else if ( strncmp( arg, "off", 3 ) == 0 ) s_log_enabled = false;
        else                                       s_log_enabled = !s_log_enabled;
        printf( "log output: %s\n", s_log_enabled ? "on" : "off" );
        return;
    }

    printf( "unknown command: '%s'  (type 'help')\n", line );
}

// ── USB / serial console task ─────────────────────────────────────────────────
// Pinned to core 0: TinyUSB IRQ fires on core 0; printf() must live there too.
//
// Loop:
//   1. Drain g_log_queue — print only when s_log_enabled.
//   2. Non-blocking char read — echo, backspace, dispatch on CR/LF.
//      Prints "# " as a prompt after every dispatched line.
static void usb_task( void* )
{
    char       line[ 128 ] = { 0 };
    size_t     line_len    = 0;

    for ( ;; ) {
        // ── 1. Drain log queue ────────────────────────────────────────────
        {
            LogMessage msg;
            if ( xQueueReceive( g_log_queue, &msg, pdMS_TO_TICKS( 10 ) ) == pdTRUE ) {
                bool flushed = false;
                if ( s_log_enabled ) { printf( "%s", msg.buf ); flushed = true; }
                while ( xQueueReceive( g_log_queue, &msg, 0 ) == pdTRUE )
                    if ( s_log_enabled ) { printf( "%s", msg.buf ); flushed = true; }
                if ( flushed ) stdio_flush();
            }
        }

        // ── 2. Read characters from USB CDC ──────────────────────────────
        {
            int c;
            while ( ( c = getchar_timeout_us( 0 ) ) != PICO_ERROR_TIMEOUT && c < 256 ) {
                if ( c == '\r' || c == '\n' ) {
                    printf( "\r\n" );
                    line[ line_len ] = '\0';
                    dispatch( line, line_len );
                    line_len  = 0;
                    line[ 0 ] = '\0';
                    printf( "# " );
                    stdio_flush();

                } else if ( ( c == '\b' || c == 127 ) && line_len > 0 ) {
                    line[ --line_len ] = '\0';
                    printf( "\b \b" );
                    stdio_flush();

                } else if ( c >= 0x20 && line_len < sizeof( line ) - 1 ) {
                    line[ line_len++ ] = ( char ) c;
                    stdio_putchar( c );
                    stdio_flush();
                }
            }
        }
    }
}

static StaticTask_t s_usb_tcb;
static StackType_t  s_usb_stack[ 2048 ];

void usb_task_init()
{
    TaskHandle_t h = xTaskCreateStatic( usb_task, "usb", 2048,
                                         nullptr, tskIDLE_PRIORITY + 1,
                                         s_usb_stack, &s_usb_tcb );
    configASSERT( h );
    // TinyUSB IRQ fires on core 0; printf must live on the same core.
    vTaskCoreAffinitySet( h, 0x01 );
}
