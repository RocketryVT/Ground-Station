// lora2_task.cpp — RFM69HCW / 424.5 MHz receive, forward to primary Pico via UDP.
//
// Mirrors lora1_task.cpp but uses the RF69 driver on SPI1.
// Each received SIGMA LoRa frame is decoded and immediately re-encoded as a
// SIGMA INTER_PICO frame and enqueued to g_udp_queue.

#include "lora2_task.hpp"
#include "shared.hpp"
#include "sigma2_forward.hpp"

#include "rf69/RF69.hpp"
#include "SIGMA.hpp"

#include "hardware/spi.h"
#include "hardware/gpio.h"

// -- Radio instance ------------------------------------------------------------
static const radio::rf69::Config s_cfg = [] {
    radio::rf69::Config cfg;
    cfg.freq_mhz  = LoRa2Cfg::FREQ_MHZ;
    cfg.br_kbps   = LoRa2Cfg::BR_KBPS;
    cfg.fdev_khz  = LoRa2Cfg::FREQ_DEV_KHZ;
    cfg.rx_bw_khz = LoRa2Cfg::RX_BW_KHZ;
    cfg.tx_dbm    = LoRa2Cfg::TX_POWER;
    cfg.preamble  = LoRa2Cfg::PREAMBLE;
    cfg.afc       = false;  // re-enable only with a wider AfcBw + longer TX preamble (AFC wasn't locking)
    return cfg;
}();

static radio::rf69::RF69 s_radio( spi1,
                                  Pins::LORA1_SCK,
                                  Pins::LORA1_MOSI,
                                  Pins::LORA1_MISO,
                                  Pins::LORA1_NSS,
                                  Pins::LORA1_DIO0,
                                  Pins::LORA1_RST,
                                  s_cfg );

// -- Task ----------------------------------------------------------------------
static void lora2_task( void* )
{
    // Assert power-enable pin
    gpio_init( Pins::LORA1_EN );
    gpio_set_dir( Pins::LORA1_EN, GPIO_OUT );
    gpio_put( Pins::LORA1_EN, 1 );

    vTaskDelay( pdMS_TO_TICKS( 500 ) );

    int state = s_radio.begin();
    if ( state != RADIOLIB_ERR_NONE ) {
        log_print( "[lora2] init failed %d at %s — task halting\n",
                   state, s_radio.init_stage() );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }
    log_print( "[lora2] RFM69 ready — %.1f MHz  %.1f kbps\n",
               LoRa2Cfg::FREQ_MHZ, LoRa2Cfg::BR_KBPS );

    s_radio.start_receive();

    for ( ;; ) {
        if ( !s_radio.packet_available() ) {
            vTaskDelay( pdMS_TO_TICKS( 5 ) );
            continue;
        }

        radio::Packet pkt;
        if ( s_radio.read_packet( pkt ) != RADIOLIB_ERR_NONE ) {
            log_print( "[lora2] readPacket error\n" );
            s_radio.start_receive();
            continue;
        }

        if ( lora_bridge::handle_sigma2_packet( pkt, "lora2" ) ) {
            s_radio.start_receive();
            continue;
        }

        // Decode the legacy SIGMA LoRa frame from the raw radio payload
        SIGMA::LoRaData d;
        if ( !SIGMA::LoRaData::deserialize( pkt.data, pkt.len, d ) ) {
            log_print( "[lora2] rx %u B  RSSI %.0f dBm — bad frame\n",
                       (unsigned)pkt.len, (double)pkt.rssi );
            s_radio.start_receive();
            continue;
        }

        log_print( "ALT,%.2f,%lu,%.1f,%.1f\n",
                   (double)d.alt_baro_m,
                   (unsigned long)d.boot_ms,
                   (double)pkt.rssi,
                   (double)pkt.snr );
        log_print( "[lora2] rx baro=%.1f m gps=%.1f m RSSI=%.0f dBm\n",
                   (double)d.alt_baro_m, (double)d.alt_gps_m, (double)pkt.rssi );

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

        s_radio.start_receive();
    }
}

static StaticTask_t s_lora2_tcb;
static StackType_t  s_lora2_stack[ 2048 ];

void lora2_task_init()
{
    task_create( lora2_task, "lora2", 2048, nullptr, tskIDLE_PRIORITY + 4,
                  s_lora2_stack, &s_lora2_tcb );
}
