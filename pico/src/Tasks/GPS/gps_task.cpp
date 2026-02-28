#include "gps_task.hpp"
#include "shared.hpp"

#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "gps/GPSParser.h"

#include <string.h>

#define GPS_UART      uart1
#define GPS_BAUD      38400
#define GPS_PUB_MS    1000   // publish rate cap (ms)

static void gps_task( void* param )
{
    ( void ) param;

    // Initialise UART1 for NMEA at 9600 8N1
    uart_init( GPS_UART, GPS_BAUD );
    gpio_set_function( Pins::GPS_UART_TX, GPIO_FUNC_UART );
    gpio_set_function( Pins::GPS_UART_RX, GPIO_FUNC_UART );
    uart_set_hw_flow( GPS_UART, false, false );
    uart_set_format( GPS_UART, 8, 1, UART_PARITY_NONE );
    uart_set_fifo_enabled( GPS_UART, true );

    log_print( "[gps] UART1 ready at %d baud (GPIO %u RX / GPIO %u TX)\n",
               GPS_BAUD, Pins::GPS_UART_RX, Pins::GPS_UART_TX );

    gps::GPSParser parser;
    TickType_t     last_pub = 0;
    bool           had_fix  = false;

    char json[ 128 ];
    char nmea_buf[ 128 ];
    int  nmea_len = 0;

    for ( ;; ) {
        // Drain UART FIFO — at 9600 baud, 10 ms ≈ 9.6 bytes; FIFO is 32 deep
        while ( uart_is_readable( GPS_UART ) ) {
            char c = ( char ) uart_getc( GPS_UART );
            parser.parse( c );

            // Echo raw NMEA to USB console
            if ( c == 0 ) {
                continue;   // ignore spurious null bytes
            } else if ( (int)c == 10 ) {   // LF — end of sentence
                nmea_buf[ nmea_len ] = '\0';
                if ( nmea_len > 0 )
                    log_print( "[gps raw] %s\n", nmea_buf );
                nmea_len = 0;
            } else if ( c != '\r' && nmea_len < ( int ) sizeof( nmea_buf ) - 1 ) {
                nmea_buf[ nmea_len++ ] = c;
            } else if ( nmea_len >= ( int ) sizeof( nmea_buf ) - 1 ) {
                nmea_len = 0;   // overflow — drop and resync
            }
        }

        if ( parser.hasFix() ) {
            const gps::Coordinate& c = parser.getCoordinate();

            if ( !had_fix ) {
                log_print( "[gps] fix acquired: %.6f, %.6f  alt %.1f m  sats %d\n",
                           c.latitude, c.longitude, c.altitude, c.satellites );
                had_fix = true;
            }

            // Keep servo task up to date — overwrite so it always sees latest
            LocationMsg loc { c.latitude, c.longitude, c.altitude };
            xQueueOverwrite( g_gs_location_q, &loc );

            TickType_t now = xTaskGetTickCount();
            if ( ( now - last_pub ) >= pdMS_TO_TICKS( GPS_PUB_MS ) ) {
                last_pub = now;

                const gps::Coordinate& c = parser.getCoordinate();
                snprintf( json, sizeof( json ),
                          "{\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"sats\":%d}",
                          c.latitude, c.longitude, c.altitude, c.satellites );

                MqttMessage msg;
                strncpy( msg.topic,   "rocket/gps", sizeof( msg.topic ) - 1 );
                strncpy( msg.payload, json,          sizeof( msg.payload ) - 1 );

                xQueueSend( g_mqtt_queue, &msg, pdMS_TO_TICKS( 50 ) );
            }
        } else if ( had_fix ) {
            log_print( "[gps] fix lost\n" );
            had_fix = false;
        }

        vTaskDelay( pdMS_TO_TICKS( 10 ) );   // 100 Hz poll — comfortably drains 9600 baud FIFO
    }
}

static StaticTask_t s_gps_tcb;
static StackType_t  s_gps_stack[ 1024 ];

void gps_task_init()
{
    configASSERT( xTaskCreateStatic( gps_task, "gps", 1024,
                                      NULL, tskIDLE_PRIORITY + 3,
                                      s_gps_stack, &s_gps_tcb ) );
}
