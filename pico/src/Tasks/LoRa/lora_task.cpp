#include "lora_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Proto/mqtt_proto.hpp"

#include "sx1276/SX1276.hpp"
#include "SIGMA.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include <string.h>

static const radio::sx1276::Config s_lora0_cfg {
    .freq_mhz  = LoRa0Cfg::FREQ_MHZ,
    .bw_khz    = LoRa0Cfg::BW_KHZ,
    .sf        = LoRa0Cfg::SF,
    .cr        = LoRa0Cfg::CR,
    .sync_word = LoRa0Cfg::SYNC_WORD,
    .tx_dbm    = LoRa0Cfg::TX_POWER,
    .preamble  = LoRa0Cfg::PREAMBLE,
};

static radio::sx1276::SX1276 s_radio(
    spi0,
    Pins::LORA0_SCK,
    Pins::LORA0_MOSI,
    Pins::LORA0_MISO,
    Pins::LORA0_NSS,
    Pins::LORA0_DIO0,
    Pins::LORA0_RST,
    s_lora0_cfg );

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

static uint8_t diag_spi_read_reg( bool miso_pullup )
{
    spi_init( spi0, 1'000'000u );
    spi_set_format( spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST );
    gpio_set_function( Pins::LORA0_SCK,  GPIO_FUNC_SPI );
    gpio_set_function( Pins::LORA0_MOSI, GPIO_FUNC_SPI );
    gpio_set_function( Pins::LORA0_MISO, GPIO_FUNC_SPI );

    if ( miso_pullup ) gpio_pull_up( Pins::LORA0_MISO );
    else               gpio_disable_pulls( Pins::LORA0_MISO );

    gpio_init( Pins::LORA0_NSS );
    gpio_set_dir( Pins::LORA0_NSS, GPIO_OUT );
    gpio_put( Pins::LORA0_NSS, 1 );

    gpio_init( Pins::LORA0_RST );
    gpio_set_dir( Pins::LORA0_RST, GPIO_OUT );
    gpio_put( Pins::LORA0_RST, 0 );
    sleep_ms( 10 );
    gpio_put( Pins::LORA0_RST, 1 );
    sleep_ms( 10 );

    gpio_put( Pins::LORA0_NSS, 0 );
    const uint8_t addr = 0x42u;
    uint8_t val = 0xAAu;
    spi_write_blocking( spi0, &addr, 1 );
    spi_read_blocking( spi0, 0x00u, &val, 1 );
    gpio_put( Pins::LORA0_NSS, 1 );

    gpio_disable_pulls( Pins::LORA0_MISO );
    return val;
}

static void diag_spi()
{
    const uint8_t no_pull = diag_spi_read_reg( false );
    const uint8_t pullup  = diag_spi_read_reg( true );

    log_print( "[lora0] SX1276 SPI diag: RegVersion=0x%02X (pulled-up=0x%02X)\n",
               no_pull, pullup );

    if ( no_pull == 0x12 ) {
        log_print( "[lora0] diag: RFM9x/SX1276 OK\n" );
    } else if ( no_pull == 0x00 && pullup == 0xFF ) {
        log_print( "[lora0] diag: MISO not connected - check GPIO%u\n", Pins::LORA0_MISO );
    } else if ( no_pull == 0x00 && pullup == 0x00 ) {
        log_print( "[lora0] diag: MISO shorted to GND\n" );
    } else if ( no_pull == 0xFF && pullup == 0xFF ) {
        log_print( "[lora0] diag: chip in reset / no power - check GPIO%u and 3.3V\n",
                   Pins::LORA0_RST );
    } else {
        log_print( "[lora0] diag: unexpected - NSS=%u SCK=%u MOSI=%u MISO=%u RST=%u\n",
                   Pins::LORA0_NSS, Pins::LORA0_SCK,
                   Pins::LORA0_MOSI, Pins::LORA0_MISO, Pins::LORA0_RST );
    }
}

static void lora0_task( void* )
{
    gpio_init( Pins::LORA0_EN );
    gpio_set_dir( Pins::LORA0_EN, GPIO_OUT );
    gpio_put( Pins::LORA0_EN, 1 );

    for ( int i = 10; i > 0; i-- ) {
        log_print( "[lora0] RFM9x init in %d s...\n", i );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }

    diag_spi();

    int state = s_radio.begin();
    if ( state != 0 ) {
        log_print( "[lora0] RFM9x/SX1276 init failed %d - task halting\n", state );
        while ( true ) vTaskDelay( portMAX_DELAY );
    }

    log_print( "[lora0] RFM9x/SX1276 ready - %.1f MHz SF%u BW%.0f kHz sync=0x%02X\n",
               LoRa0Cfg::FREQ_MHZ, (unsigned)LoRa0Cfg::SF,
               LoRa0Cfg::BW_KHZ, (unsigned)LoRa0Cfg::SYNC_WORD );

    s_radio.start_receive();

    for ( ;; ) {
        if ( s_radio.packet_available() ) {
            radio::Packet pkt;
            state = s_radio.read_packet( pkt );

            if ( state == 0 ) {
                SigmaLoRaData d;
                if ( SigmaLoRaData::deserialize( pkt.data, pkt.len, d ) ) {
                    log_print( "[lora0] %lu,%s,%u,%u,%.7f,%.7f,%.1f,%.1f,%.2f,"
                               "%.5f,%.5f,%.5f,%.5f,%.1f,%.1f\n",
                               (unsigned long)d.boot_ms,
                               state_name( d.state ),
                               (unsigned)d.satellites,
                               (unsigned)d.flags,
                               d.lat, d.lon,
                               (double)d.alt_gps_m, (double)d.alt_baro_m,
                               (double)d.speed_ms,
                               (double)d.q[0], (double)d.q[1],
                               (double)d.q[2], (double)d.q[3],
                               (double)pkt.rssi, (double)pkt.snr );

                    if ( mqtt_is_connected() ) {
                        MqttMessage m = {};
                        groundstation_RocketLoRaSample pb =
                            groundstation_RocketLoRaSample_init_zero;
                        pb.has_boot_ms = true;   pb.boot_ms = d.boot_ms;
                        pb.has_state = true;     pb.state = (groundstation_FlightState)d.state;
                        pb.has_sats = true;      pb.sats = d.satellites;
                        pb.has_flags = true;     pb.flags = d.flags;
                        pb.has_lat = true;       pb.lat = d.lat;
                        pb.has_lon = true;       pb.lon = d.lon;
                        pb.has_alt_gps_m = true; pb.alt_gps_m = d.alt_gps_m;
                        pb.has_alt_baro_m = true; pb.alt_baro_m = d.alt_baro_m;
                        pb.has_speed_ms = true;  pb.speed_ms = d.speed_ms;
                        pb.q_count = 4;
                        pb.q[0] = d.q[0]; pb.q[1] = d.q[1];
                        pb.q[2] = d.q[2]; pb.q[3] = d.q[3];
                        pb.has_rssi = true;      pb.rssi = pkt.rssi;
                        pb.has_snr = true;       pb.snr = pkt.snr;

                        if ( mqtt_encode_proto( m, "rocket/lora0",
                                                groundstation_RocketLoRaSample_fields,
                                                &pb ) )
                            xQueueSend( g_mqtt_queue, &m, 0 );
                    }

                    if ( d.flags & SIGMA_FLAG_GPS_VALID ) {
                        LocationMsg loc = { d.lat, d.lon, (double)d.alt_gps_m };
                        xQueueOverwrite( g_rocket_location_q, &loc );
                    }
                } else {
                    log_print( "[lora0] rx %u B RSSI %.1f dBm SNR %.1f dB - bad SIGMA frame\n",
                               (unsigned)pkt.len, (double)pkt.rssi, (double)pkt.snr );
                }
            } else {
                log_print( "[lora0] read_packet error %d\n", state );
            }

            s_radio.start_receive();
        }

        vTaskDelay( pdMS_TO_TICKS( 10 ) );
    }
}

static StaticTask_t s_lora0_tcb;
static StackType_t  s_lora0_stack[ 2048 ];

void lora0_task_init()
{
    TaskHandle_t h = task_create( lora0_task, "lora0", 2048,
                                  nullptr, tskIDLE_PRIORITY + 4,
                                  s_lora0_stack, &s_lora0_tcb );
    vTaskCoreAffinitySet( h, 1u << 0 );
}
