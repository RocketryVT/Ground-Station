#include "lora_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include "SX1276Radio.hpp"
#include "SIGMA.hpp"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"
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

// ── SPI diagnostic ────────────────────────────────────────────────────────────
// Reads the SX1276 RegVersion register (0x42) via bare-metal SPI before
// RadioLib's begin(), so the raw byte is logged regardless of outcome.
// A second read with MISO pulled up disambiguates floating from driven-low:
//   read=0x12, pulled=0x12 → SX1276 present and healthy
//   read=0x00, pulled=0xFF → MISO pin not connected (wire missing)
//   read=0x00, pulled=0x00 → MISO shorted to GND  (short circuit)
//   read=0xFF, pulled=0xFF → chip in reset / not powered (RST stuck low, or no 3.3V)
static uint8_t diag_spi_read_reg( bool miso_pullup )
{
    spi_init( spi0, 1'000'000u );
    spi_set_format( spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST );
    gpio_set_function( Pins::LORA_SCK,  GPIO_FUNC_SPI );
    gpio_set_function( Pins::LORA_MOSI, GPIO_FUNC_SPI );
    gpio_set_function( Pins::LORA_MISO, GPIO_FUNC_SPI );

    if ( miso_pullup )
        gpio_pull_up( Pins::LORA_MISO );
    else
        gpio_disable_pulls( Pins::LORA_MISO );

    gpio_init( Pins::LORA_NSS );
    gpio_set_dir( Pins::LORA_NSS, GPIO_OUT );
    gpio_put( Pins::LORA_NSS, 1 );

    // RST — pulse low then release to bring chip out of reset
    gpio_init( Pins::LORA_RST );
    gpio_set_dir( Pins::LORA_RST, GPIO_OUT );
    gpio_put( Pins::LORA_RST, 0 );
    sleep_ms( 10 );
    gpio_put( Pins::LORA_RST, 1 );
    sleep_ms( 10 );   // 5 ms POR settle per SX1276 datasheet

    gpio_put( Pins::LORA_NSS, 0 );   // assert NSS
    const uint8_t addr = 0x42u;      // RegVersion — bit7=0 = read
    uint8_t val = 0xAAu;
    spi_write_blocking( spi0, &addr, 1 );
    spi_read_blocking(  spi0, 0x00u, &val, 1 );
    gpio_put( Pins::LORA_NSS, 1 );

    gpio_disable_pulls( Pins::LORA_MISO );
    return val;
}

static void diag_spi( void )
{
    uint8_t no_pull  = diag_spi_read_reg( false );
    uint8_t pullup   = diag_spi_read_reg( true  );

    log_print( "[lora] SPI diag: RegVersion=0x%02X  (pulled-up=0x%02X)\n",
               no_pull, pullup );

    if ( no_pull == 0x12 ) {
        log_print( "[lora] diag: SX1276 OK\n" );
    } else if ( no_pull == 0x00 && pullup == 0xFF ) {
        log_print( "[lora] diag: MISO NOT CONNECTED — check wiring on GPIO%u\n",
                   Pins::LORA_MISO );
    } else if ( no_pull == 0x00 && pullup == 0x00 ) {
        log_print( "[lora] diag: MISO SHORTED TO GND\n" );
    } else if ( no_pull == 0xFF && pullup == 0xFF ) {
        log_print( "[lora] diag: chip in reset or not powered — check RST (GPIO%u) and 3.3V\n",
                   Pins::LORA_RST );
    } else {
        log_print( "[lora] diag: unexpected — NSS=GPIO%u SCK=GPIO%u MOSI=GPIO%u MISO=GPIO%u RST=GPIO%u\n",
                   Pins::LORA_NSS, Pins::LORA_SCK,
                   Pins::LORA_MOSI, Pins::LORA_MISO, Pins::LORA_RST );
    }
}

// ── Task ─────────────────────────────────────────────────────────────────────
void lora_task( void* param )
{
    ( void ) param;

    // Wait for 10 seconds before starting radio init to allow other tasks to start up and
    for ( int i = 0; i < 10; i++ ) {
        log_print( "[lora] init in %d seconds...\n", 10 - i );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }

    // Bare-metal SPI read of RegVersion before RadioLib — disambiguates
    // wiring faults before RadioLib consumes the error silently.
    diag_spi();

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

                    // ── Publish to MQTT broker ────────────────────────────
                    if ( mqtt_is_connected() ) {
                        MqttMessage m;
                        strncpy( m.topic, "rocket/lora", sizeof( m.topic ) );
                        snprintf( m.payload, sizeof( m.payload ),
                                  "{\"boot_ms\":%lu,\"state\":\"%s\","
                                  "\"sats\":%u,\"flags\":%u,"
                                  "\"lat\":%.7f,\"lon\":%.7f,"
                                  "\"alt_gps_m\":%.1f,\"alt_baro_m\":%.1f,"
                                  "\"speed_ms\":%.2f,"
                                  "\"q\":[%.5f,%.5f,%.5f,%.5f],"
                                  "\"rssi\":%.1f,\"snr\":%.1f}",
                                  ( unsigned long ) d.boot_ms,
                                  state_name( d.state ),
                                  ( unsigned ) d.satellites,
                                  ( unsigned ) d.flags,
                                  d.lat, d.lon,
                                  ( double ) d.alt_gps_m,
                                  ( double ) d.alt_baro_m,
                                  ( double ) d.speed_ms,
                                  ( double ) d.q[0], ( double ) d.q[1],
                                  ( double ) d.q[2], ( double ) d.q[3],
                                  ( double ) pkt.rssi,
                                  ( double ) pkt.snr );
                        xQueueSend( g_mqtt_queue, &m, 0 );
                    }

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
    TaskHandle_t lora_handle = xTaskCreateStatic( lora_task, "lora", 2048,
                                                   NULL, tskIDLE_PRIORITY + 4,
                                                   s_lora_stack, &s_lora_tcb );
    configASSERT( lora_handle );
    vTaskCoreAffinitySet( lora_handle, ( 1u << 0 ) );
}
