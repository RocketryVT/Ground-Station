#include "lora_task.hpp"
#include "shared.hpp"

#include "SX1276Radio.hpp"
#include "SIGMA.hpp"

#include <string.h>

// ── Radio instance (static — RadioLib keeps internal references) ──────────────
static SX1276Radio s_radio( spi0,
                             Pins::LORA_SCK,
                             Pins::LORA_MOSI,
                             Pins::LORA_MISO,
                             Pins::LORA_NSS,
                             Pins::LORA_DIO0,
                             Pins::LORA_RST );

// ── Flight state name ─────────────────────────────────────────────────────────
static const char* state_name( FlightState s )
{
    switch ( s ) {
        case FlightState::GROUND_IDLE:    return "GROUND_IDLE";
        case FlightState::ARMED:          return "ARMED";
        case FlightState::POWERED_ASCENT: return "POWERED_ASCENT";
        case FlightState::COAST_ASCENT:   return "COAST_ASCENT";
        case FlightState::APOGEE:         return "APOGEE";
        case FlightState::DESCENT_DROGUE: return "DESCENT_DROGUE";
        case FlightState::DESCENT_MAIN:   return "DESCENT_MAIN";
        case FlightState::LANDED:         return "LANDED";
        case FlightState::FAULT:          return "FAULT";
        default:                          return "UNKNOWN";
    }
}

// ── Task ─────────────────────────────────────────────────────────────────────
void lora_task( void* param )
{
    ( void ) param;

    SX1276Config cfg {
        LoRaCfg::FREQ_MHZ,
        LoRaCfg::BW_KHZ,
        LoRaCfg::SF,
        LoRaCfg::CR,
        LoRaCfg::SYNC_WORD,
        LoRaCfg::TX_POWER,
        LoRaCfg::PREAMBLE
    };

    int state = s_radio.begin( cfg );

    if ( state != LORA_OK ) {
        log_print( "[lora] init failed %d — task halting\n", state );
        while ( true ) vTaskDelay( portMAX_DELAY );
    }

    log_print( "[lora] SX1276 ready — %.1f MHz  SF%u  BW%.0f kHz  sync=0x%02X\n",
               LoRaCfg::FREQ_MHZ, ( unsigned ) LoRaCfg::SF,
               LoRaCfg::BW_KHZ,  ( unsigned ) LoRaCfg::SYNC_WORD );

    // CSV header — printed once so output can be pasted into a spreadsheet
    // or piped to a file without post-processing.
    log_print( "boot_ms,state,satellites,flags,"
               "lat,lon,alt_gps_m,alt_baro_m,speed_ms,"
               "q_w,q_x,q_y,q_z,"
               "rssi_dBm,snr_dB\n" );

    s_radio.startReceive();

    for ( ;; ) {
        if ( s_radio.packetAvailable() ) {
            LoRaPacket pkt;
            state = s_radio.readPacket( pkt );

            if ( state == LORA_OK ) {
                SigmaLoRaData d;
                if ( SigmaLoRaData::deserialize(
                         reinterpret_cast<const uint8_t*>( pkt.data ),
                         pkt.len, d ) )
                {
                    // ── CSV row ───────────────────────────────────────────
                    log_print( "%lu,%s,%u,%u,"
                               "%.7f,%.7f,%.1f,%.1f,%.2f,"
                               "%.5f,%.5f,%.5f,%.5f,"
                               "%.1f,%.1f\n",
                               ( unsigned long ) d.boot_ms,
                               state_name( d.state ),
                               ( unsigned ) d.satellites,
                               ( unsigned ) d.flags,
                               d.lat,
                               d.lon,
                               ( double ) d.alt_gps_m,
                               ( double ) d.alt_baro_m,
                               ( double ) d.speed_ms,
                               ( double ) d.q[0],
                               ( double ) d.q[1],
                               ( double ) d.q[2],
                               ( double ) d.q[3],
                               ( double ) pkt.rssi,
                               ( double ) pkt.snr );

                    // ── Update antenna tracker location ───────────────────
                    if ( d.flags & SIGMA_FLAG_GPS_VALID ) {
                        LocationMsg loc = {
                            .lat   = d.lat,
                            .lon   = d.lon,
                            .alt_m = ( double ) d.alt_gps_m,
                        };
                        xQueueOverwrite( g_rocket_location_q, &loc );
                    }
                } else {
                    // Frame received but SIGMA framing/CRC check failed.
                    log_print( "[lora] rx %u bytes  RSSI %.1f dBm  SNR %.1f dB"
                               "  — bad SIGMA frame\n",
                               ( unsigned ) pkt.len,
                               ( double ) pkt.rssi,
                               ( double ) pkt.snr );
                }
            } else {
                log_print( "[lora] readPacket error %d\n", state );
            }

            s_radio.startReceive();
        }

        vTaskDelay( pdMS_TO_TICKS( 10 ) );   // 100 Hz poll — yields to other tasks
    }
}

static StaticTask_t s_lora_tcb;
static StackType_t  s_lora_stack[ 2048 ];

void lora_task_init()
{
    configASSERT( xTaskCreateStatic( lora_task, "lora", 2048,
                                      NULL, tskIDLE_PRIORITY + 4,
                                      s_lora_stack, &s_lora_tcb ) );
}
