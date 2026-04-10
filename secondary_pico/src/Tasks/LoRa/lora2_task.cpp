// lora2_task.cpp — RFM69HCW / 433 MHz receive, forward to primary Pico via UDP.
//
// Mirrors lora1_task.cpp but uses the RF69Radio driver on SPI1.
// Each received SIGMA LoRa frame is decoded and immediately re-encoded as a
// SIGMA INTER_PICO frame and enqueued to g_udp_queue.

#include "lora2_task.hpp"
#include "shared.hpp"

#include "radio/RF69Radio.hpp"
#include "SIGMA.hpp"

#include "hardware/spi.h"
#include "hardware/gpio.h"

// -- Radio instance ------------------------------------------------------------
static const RF69Config s_cfg {
    LoRa2Cfg::FREQ_MHZ,
    LoRa2Cfg::BR_KBPS,
    LoRa2Cfg::FREQ_DEV_KHZ,
    LoRa2Cfg::RX_BW_KHZ,
    LoRa2Cfg::TX_POWER,
    LoRa2Cfg::PREAMBLE,
};

static RF69Radio s_radio( spi1,
                            Pins::LORA2_SCK,
                            Pins::LORA2_MOSI,
                            Pins::LORA2_MISO,
                            Pins::LORA2_NSS,
                            Pins::LORA2_DIO0,
                            Pins::LORA2_RST,
                            s_cfg );

// -- Task ----------------------------------------------------------------------
static void lora2_task( void* )
{
    // Assert power-enable pin
    gpio_init( Pins::LORA2_EN );
    gpio_set_dir( Pins::LORA2_EN, GPIO_OUT );
    gpio_put( Pins::LORA2_EN, 1 );

    vTaskDelay( pdMS_TO_TICKS( 500 ) );

    int state = s_radio.begin();
    if ( state != RADIO_OK ) {
        log_print( "[lora2] init failed %d — task halting\n", state );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }
    log_print( "[lora2] RFM69 ready — %.1f MHz  %.1f kbps\n",
               LoRa2Cfg::FREQ_MHZ, LoRa2Cfg::BR_KBPS );

    s_radio.startReceive();

    for ( ;; ) {
        if ( !s_radio.packetAvailable() ) {
            vTaskDelay( pdMS_TO_TICKS( 5 ) );
            continue;
        }

        RadioPacket pkt;
        if ( s_radio.readPacket( pkt ) != RADIO_OK ) {
            log_print( "[lora2] readPacket error\n" );
            s_radio.startReceive();
            continue;
        }

        // Decode the SIGMA LoRa frame from the raw radio payload
        SIGMA::LoRaData d;
        if ( !SIGMA::LoRaData::deserialize( pkt.data, pkt.len, d ) ) {
            log_print( "[lora2] rx %u B  RSSI %.0f dBm — bad frame\n",
                       (unsigned)pkt.len, (double)pkt.rssi );
            s_radio.startReceive();
            continue;
        }

        log_print( "[lora2] rx lat=%.5f lon=%.5f alt_gps=%.0f m  RSSI=%.0f dBm\n",
                   d.lat, d.lon, (double)d.alt_gps_m, (double)pkt.rssi );

        // SNR is not available on FSK/OOK radios — RF69 reports 0.0
        SIGMA::InterPicoData fwd = SIGMA::InterPicoData::from_lora(
            d, static_cast<int>(pkt.rssi), pkt.snr );

        UdpFrame frame;
        frame.len = static_cast<uint16_t>(
            fwd.serialize( frame.data, sizeof(frame.data) ) );

        if ( frame.len > 0 ) {
            if ( xQueueSend( g_udp_queue, &frame, 0 ) != pdTRUE ) {
                log_print( "[lora2] udp queue full — frame dropped\n" );
            }
        }

        s_radio.startReceive();
    }
}

static StaticTask_t s_lora2_tcb;
static StackType_t  s_lora2_stack[ 2048 ];

void lora2_task_init()
{
    task_create( lora2_task, "lora2", 2048, nullptr, tskIDLE_PRIORITY + 4,
                  s_lora2_stack, &s_lora2_tcb );
}
