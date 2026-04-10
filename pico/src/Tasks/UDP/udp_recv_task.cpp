// udp_recv_task.cpp — receive SIGMA INTER_PICO frames from secondary Pico.
//
// Architecture (NO_SYS=1 / threadsafe-background):
//   - lwIP UDP callback fires in IRQ context; copies raw bytes to s_rx_queue.
//   - FreeRTOS task wakes, decodes the SIGMA frame, updates queues.
//
// On each valid frame:
//   g_rocket_location_q  ← xQueueOverwrite (antenna tracker reads this)
//   g_mqtt_queue         ← xQueueSend (laptop visualization, best-effort)

#include "udp_recv_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include "SIGMA.hpp"

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#include <string.h>

// -- Internal raw-frame queue (IRQ -> task) ------------------------------------
// The lwIP callback memcpy's the incoming UDP payload here; the task decodes.
#define RX_QUEUE_DEPTH  8
#define RX_BUF_SIZE     64   // larger than max InterPico frame (49 B)

struct RxRawFrame {
    uint8_t  data[ RX_BUF_SIZE ];
    uint16_t len;
};

static StaticQueue_t s_rx_queue_buf;
static uint8_t       s_rx_queue_storage[ RX_QUEUE_DEPTH * sizeof(RxRawFrame) ];
static QueueHandle_t s_rx_queue = nullptr;

// -- lwIP UDP receive callback (IRQ context) -----------------------------------
static void on_udp_recv( void*           arg,
                          struct udp_pcb* pcb,
                          struct pbuf*    p,
                          const ip_addr_t* addr,
                          uint16_t        port )
{
    ( void ) arg; ( void ) pcb; ( void ) addr; ( void ) port;

    if ( !p ) return;

    RxRawFrame frame;
    frame.len = ( p->tot_len < RX_BUF_SIZE ) ? p->tot_len : RX_BUF_SIZE;
    pbuf_copy_partial( p, frame.data, frame.len, 0 );
    pbuf_free( p );

    // Post to FreeRTOS queue from IRQ — use FromISR variant
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR( s_rx_queue, &frame, &woken );
    portYIELD_FROM_ISR( woken );
}

// -- Helpers -------------------------------------------------------------------
static const char* flight_state_str( SIGMA::FlightState s )
{
    switch ( s ) {
        case SIGMA::FlightState::GROUND_IDLE:    return "GROUND_IDLE";
        case SIGMA::FlightState::ARMED:          return "ARMED";
        case SIGMA::FlightState::POWERED_ASCENT: return "POWERED_ASCENT";
        case SIGMA::FlightState::COAST_ASCENT:   return "COAST_ASCENT";
        case SIGMA::FlightState::APOGEE:         return "APOGEE";
        case SIGMA::FlightState::DESCENT_DROGUE: return "DESCENT_DROGUE";
        case SIGMA::FlightState::DESCENT_MAIN:   return "DESCENT_MAIN";
        case SIGMA::FlightState::LANDED:         return "LANDED";
        case SIGMA::FlightState::FAULT:          return "FAULT";
        default:                                 return "UNKNOWN";
    }
}

// -- Task ----------------------------------------------------------------------
static void udp_recv_task( void* )
{
    log_print( "[udp_rx] task started — waiting for WiFi\n" );

    xEventGroupWaitBits( g_net_events, EVT_WIFI_CONNECTED,
                         pdFALSE, pdTRUE, portMAX_DELAY );

    // Bind UDP PCB to INTER_PICO_PORT on all interfaces
    cyw43_arch_lwip_begin();
    struct udp_pcb* pcb = udp_new();
    err_t err = ERR_OK;
    if ( pcb ) {
        err = udp_bind( pcb, IP_ADDR_ANY, INTER_PICO_PORT );
        if ( err == ERR_OK ) {
            udp_recv( pcb, on_udp_recv, nullptr );
        }
    }
    cyw43_arch_lwip_end();

    if ( !pcb || err != ERR_OK ) {
        log_print( "[udp_rx] bind failed (pcb=%p err=%d) — task halting\n",
                   pcb, (int)err );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }

    log_print( "[udp_rx] listening on port %u\n", (unsigned)INTER_PICO_PORT );

    for ( ;; ) {
        RxRawFrame raw;
        if ( xQueueReceive( s_rx_queue, &raw, pdMS_TO_TICKS( 200 ) ) != pdTRUE )
            continue;

        SIGMA::InterPicoData d;
        if ( !SIGMA::InterPicoData::deserialize( raw.data, raw.len, d ) ) {
            log_print( "[udp_rx] bad INTER_PICO frame (%u B)\n", (unsigned)raw.len );
            continue;
        }

        log_print( "[udp_rx] lat=%.5f lon=%.5f alt_gps=%.0f m  RSSI=%d dBm  SNR=%.1f dB\n",
                   d.lat, d.lon, (double)d.alt_gps_m,
                   d.rssi, (double)d.snr_dB );

        // Update rocket location queue (antenna tracker reads this)
        if ( d.flags & SIGMA::FLAG_GPS_VALID ) {
            LocationMsg loc { d.lat, d.lon, (double)d.alt_gps_m };
            xQueueOverwrite( g_rocket_location_q, &loc );
        }

        // MQTT publish (best-effort — silently dropped if queue full or disconnected)
        if ( mqtt_is_connected() ) {
            MqttMessage m;
            strncpy( m.topic, "rocket/inter_pico", sizeof(m.topic) );
            snprintf( m.payload, sizeof(m.payload),
                      "{\"boot_ms\":%lu,\"state\":\"%s\","
                      "\"sats\":%u,\"flags\":%u,"
                      "\"lat\":%.7f,\"lon\":%.7f,"
                      "\"alt_gps_m\":%.1f,\"alt_baro_m\":%.1f,"
                      "\"speed_ms\":%.2f,"
                      "\"q\":[%.5f,%.5f,%.5f,%.5f],"
                      "\"rssi\":%d,\"snr\":%.2f}",
                      (unsigned long)d.boot_ms,
                      flight_state_str(d.state),
                      (unsigned)d.satellites,
                      (unsigned)d.flags,
                      d.lat, d.lon,
                      (double)d.alt_gps_m, (double)d.alt_baro_m,
                      (double)d.speed_ms,
                      (double)d.q[0], (double)d.q[1],
                      (double)d.q[2], (double)d.q[3],
                      d.rssi, (double)d.snr_dB );
            xQueueSend( g_mqtt_queue, &m, 0 );
        }
    }
}

static StaticTask_t s_udp_recv_tcb;
static StackType_t  s_udp_recv_stack[ 1536 ];

void udp_recv_task_init()
{
    s_rx_queue = xQueueCreateStatic( RX_QUEUE_DEPTH, sizeof(RxRawFrame),
                                      s_rx_queue_storage, &s_rx_queue_buf );

    task_create( udp_recv_task, "udp_rx", 1536, nullptr, tskIDLE_PRIORITY + 3,
                  s_udp_recv_stack, &s_udp_recv_tcb );
}
