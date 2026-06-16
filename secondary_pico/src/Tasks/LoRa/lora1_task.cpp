// lora1_task.cpp — SX1276 / 915 MHz receive, forward to primary Pico via UDP.
//
// Each received SIGMA LoRa frame is decoded and immediately re-encoded as a
// SIGMA INTER_PICO frame (which appends RSSI + SNR from this radio) and
// enqueued to g_udp_queue.  No buffering or field-gathering: the UDP task
// drains the queue as fast as the network allows.

#include "lora1_task.hpp"
#include "shared.hpp"
#include "sigma2_forward.hpp"

#include "sx1276/SX1276.hpp"
#include "SIGMA.hpp"

#include "hardware/spi.h"
#include "hardware/gpio.h"

// -- Radio instance ------------------------------------------------------------
static const radio::sx1276::Config s_cfg = [] {
    radio::sx1276::Config cfg;
    cfg.freq_mhz  = LoRa1Cfg::FREQ_MHZ;
    cfg.bw_khz    = LoRa1Cfg::BW_KHZ;
    cfg.sf        = LoRa1Cfg::SF;
    cfg.cr        = LoRa1Cfg::CR;
    cfg.sync_word = LoRa1Cfg::SYNC_WORD;
    cfg.tx_dbm    = LoRa1Cfg::TX_POWER;
    cfg.preamble  = LoRa1Cfg::PREAMBLE;
    return cfg;
}();

static radio::sx1276::SX1276 s_radio( spi0,
                                       Pins::LORA0_SCK,
                                       Pins::LORA0_MOSI,
                                       Pins::LORA0_MISO,
                                       Pins::LORA0_NSS,
                                       Pins::LORA0_DIO0,
                                       Pins::LORA0_RST,
                                       s_cfg );

// -- Task ----------------------------------------------------------------------
static void lora1_task( void* )
{
    // Assert power-enable pin
    gpio_init( Pins::LORA0_EN );
    gpio_set_dir( Pins::LORA0_EN, GPIO_OUT );
    gpio_put( Pins::LORA0_EN, 1 );

    vTaskDelay( pdMS_TO_TICKS( 500 ) );

    int state = s_radio.begin();
    if ( state != RADIOLIB_ERR_NONE ) {
        log_print( "[lora1] init failed %d — task halting\n", state );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }
    log_print( "[lora1] SX1276 ready — %.1f MHz  SF%u  BW%.0f kHz\n",
               LoRa1Cfg::FREQ_MHZ, (unsigned)LoRa1Cfg::SF, LoRa1Cfg::BW_KHZ );

    s_radio.start_receive();

    for ( ;; ) {
        if ( !s_radio.packet_available() ) {
            vTaskDelay( pdMS_TO_TICKS( 5 ) );
            continue;
        }

        radio::Packet pkt;
        if ( s_radio.read_packet( pkt ) != RADIOLIB_ERR_NONE ) {
            log_print( "[lora1] readPacket error\n" );
            s_radio.start_receive();
            continue;
        }

        if ( lora_bridge::handle_sigma2_packet( pkt, "lora1" ) ) {
            s_radio.start_receive();
            continue;
        }

        // Decode the legacy SIGMA LoRa frame from the raw radio payload
        SIGMA::LoRaData d;
        if ( !SIGMA::LoRaData::deserialize( pkt.data, pkt.len, d ) ) {
            log_print( "[lora1] rx %u B  RSSI %.0f dBm  SNR %.1f dB — bad frame\n",
                       (unsigned)pkt.len, (double)pkt.rssi, (double)pkt.snr );
            s_radio.start_receive();
            continue;
        }

        log_print( "[lora1] rx lat=%.5f lon=%.5f alt_gps=%.0f m"
                   "  RSSI=%.0f dBm  SNR=%.1f dB\n",
                   d.lat, d.lon, (double)d.alt_gps_m,
                   (double)pkt.rssi, (double)pkt.snr );

        // Re-encode as INTER_PICO and push to UDP queue.
        // InterPicoData::from_lora() copies all LoRa fields and appends
        // RSSI / SNR measured by this receiver.
        SIGMA::InterPicoData fwd = SIGMA::InterPicoData::from_lora(
            d, static_cast<int>(pkt.rssi), pkt.snr );

        UdpFrame frame;
        frame.len = static_cast<uint16_t>(
            fwd.serialize( frame.data, sizeof(frame.data) ) );

        if ( frame.len > 0 ) {
            if ( xQueueSend( g_udp_queue, &frame, 0 ) != pdTRUE ) {
                log_print( "[lora1] udp queue full — frame dropped\n" );
            }
        }

        s_radio.start_receive();
    }
}

static StaticTask_t s_lora1_tcb;
static StackType_t  s_lora1_stack[ 2048 ];

void lora1_task_init()
{
    task_create( lora1_task, "lora1", 2048, nullptr, tskIDLE_PRIORITY + 4,
                  s_lora1_stack, &s_lora1_tcb );
}
