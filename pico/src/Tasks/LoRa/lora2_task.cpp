// #include "lora2_task.hpp"
// #include "shared.hpp"
// #include "Tasks/MQTT/mqtt_task.hpp"

// // #include "radio/RF69Radio.hpp"

// #include "hardware/spi.h"
// #include "hardware/gpio.h"
// #include <string.h>

// // -- Radio instance ------------------------------------------------------------
// static const RF69Config s_lora2_cfg {
//     LoRa2Cfg::FREQ_MHZ,
//     LoRa2Cfg::BR_KBPS,
//     LoRa2Cfg::FREQ_DEV_KHZ,
//     LoRa2Cfg::RX_BW_KHZ,
//     LoRa2Cfg::TX_POWER,
//     true,               // high_power — RFM69HCW PA boost
//     LoRa2Cfg::PREAMBLE,
// };

// static RF69Radio s_radio( spi1,
//                             Pins::LORA2_SCK,
//                             Pins::LORA2_MOSI,
//                             Pins::LORA2_MISO,
//                             Pins::LORA2_NSS,
//                             Pins::LORA2_DIO0,
//                             Pins::LORA2_RST,
//                             s_lora2_cfg );

// // -- Task ----------------------------------------------------------------------
// static void lora2_task( void* )
// {
//     // Assert power-enable pin
//     gpio_init( Pins::LORA2_EN );
//     gpio_set_dir( Pins::LORA2_EN, GPIO_OUT );
//     gpio_put( Pins::LORA2_EN, 1 );

//     // Staggered start
//     for ( int i = 12; i > 0; i-- ) {
//         log_print( "[lora2] init in %d s...\n", i );
//         vTaskDelay( pdMS_TO_TICKS( 1000 ) );
//     }

//     int state = s_radio.begin();
//     if ( state != RADIO_OK ) {
//         log_print( "[lora2] RFM69HCW init failed %d — task halting\n", state );
//         while ( true ) vTaskDelay( portMAX_DELAY );
//     }

//     log_print( "[lora2] RFM69HCW ready — %.1f MHz  %.1f kbps\n",
//                LoRa2Cfg::FREQ_MHZ, LoRa2Cfg::BR_KBPS );

//     s_radio.startReceive();

//     for ( ;; ) {
//         if ( s_radio.packetAvailable() ) {
//             RadioPacket pkt;
//             state = s_radio.readPacket( pkt );

//             if ( state == RADIO_OK && pkt.len > 0 ) {
//                 log_print( "[lora2] rx %u B  RSSI %.1f dBm\n",
//                            (unsigned)pkt.len, (double)pkt.rssi );

//                 if ( mqtt_is_connected() ) {
//                     MqttMessage m;
//                     strncpy( m.topic, "rocket/rf69", sizeof(m.topic) );
//                     // Publish raw hex payload — application layer TBD
//                     int off = 0;
//                     for ( uint8_t i = 0; i < pkt.len && off < (int)sizeof(m.payload) - 3; i++ )
//                         off += snprintf( m.payload + off, sizeof(m.payload) - off,
//                                          "%02X", (unsigned)pkt.data[i] );
//                     xQueueSend( g_mqtt_queue, &m, 0 );
//                 }
//             } else if ( state != RADIO_OK ) {
//                 log_print( "[lora2] readPacket error %d\n", state );
//             }

//             s_radio.startReceive();
//         }

//         vTaskDelay( pdMS_TO_TICKS( 10 ) );
//     }
// }

// static StaticTask_t s_lora2_tcb;
// static StackType_t  s_lora2_stack[ 1024 ];

// void lora2_task_init()
// {
//     TaskHandle_t h = xTaskCreateStatic( lora2_task, "lora2", 1024,
//                                          nullptr, tskIDLE_PRIORITY + 4,
//                                          s_lora2_stack, &s_lora2_tcb );
//     configASSERT( h );
//     vTaskCoreAffinitySet( h, 1u << 1 );   // pin to core 1, opposite of lora1
// }
