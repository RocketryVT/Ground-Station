#include "lora1_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include "rf69/RF69.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include <stdio.h>
#include <string.h>

static const radio::rf69::Config s_lora1_cfg {
    .freq_mhz   = LoRa1Cfg::FREQ_MHZ,
    .br_kbps    = LoRa1Cfg::BR_KBPS,
    .fdev_khz   = LoRa1Cfg::FREQ_DEV_KHZ,
    .rx_bw_khz  = LoRa1Cfg::RX_BW_KHZ,
    .tx_dbm     = LoRa1Cfg::TX_POWER,
    .high_power = true,
    .preamble   = LoRa1Cfg::PREAMBLE,
};

static radio::rf69::RF69 s_radio(
    spi1,
    Pins::LORA1_SCK,
    Pins::LORA1_MOSI,
    Pins::LORA1_MISO,
    Pins::LORA1_NSS,
    Pins::LORA1_DIO0,
    Pins::LORA1_RST,
    s_lora1_cfg );

static void publish_hex_packet( const radio::Packet& pkt )
{
    if ( !mqtt_is_connected() ) return;

    MqttMessage m = {};
    strncpy( m.topic, "rocket/lora1/rf69", sizeof(m.topic) - 1 );

    int off = 0;
    for ( uint8_t i = 0; i < pkt.len && off < (int)sizeof(m.payload) - 3; i++ ) {
        off += snprintf( m.payload + off, sizeof(m.payload) - off,
                         "%02X", (unsigned)pkt.data[i] );
    }

    xQueueSend( g_mqtt_queue, &m, 0 );
}

static void lora1_task( void* )
{
    gpio_init( Pins::LORA1_EN );
    gpio_set_dir( Pins::LORA1_EN, GPIO_OUT );
    gpio_put( Pins::LORA1_EN, 1 );

    for ( int i = 12; i > 0; i-- ) {
        log_print( "[lora1] RFM69HCW init in %d s...\n", i );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }

    int state = s_radio.begin();
    if ( state != 0 ) {
        log_print( "[lora1] RFM69HCW init failed %d - task halting\n", state );
        while ( true ) vTaskDelay( portMAX_DELAY );
    }

    log_print( "[lora1] RFM69HCW ready - %.1f MHz %.1f kbps fdev=%.1f kHz rx_bw=%.0f kHz\n",
               LoRa1Cfg::FREQ_MHZ, LoRa1Cfg::BR_KBPS,
               LoRa1Cfg::FREQ_DEV_KHZ, LoRa1Cfg::RX_BW_KHZ );

    s_radio.start_receive();

    for ( ;; ) {
        if ( s_radio.packet_available() ) {
            radio::Packet pkt;
            state = s_radio.read_packet( pkt );

            if ( state == 0 && pkt.len > 0 ) {
                log_print( "[lora1] RFM69 rx %u B RSSI %.1f dBm\n",
                           (unsigned)pkt.len, (double)pkt.rssi );
                publish_hex_packet( pkt );
            } else if ( state != 0 ) {
                log_print( "[lora1] read_packet error %d\n", state );
            }

            s_radio.start_receive();
        }

        vTaskDelay( pdMS_TO_TICKS( 10 ) );
    }
}

static StaticTask_t s_lora1_tcb;
static StackType_t  s_lora1_stack[ 1024 ];

void lora1_task_init()
{
    TaskHandle_t h = task_create( lora1_task, "lora1", 1024,
                                  nullptr, tskIDLE_PRIORITY + 4,
                                  s_lora1_stack, &s_lora1_tcb );
    vTaskCoreAffinitySet( h, 1u << 1 );
}
