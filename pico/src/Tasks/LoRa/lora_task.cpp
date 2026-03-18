// #include "lora_task.hpp"
// #include "shared.hpp"
// #include "Tasks/MQTT/mqtt_task.hpp"

// // #include "radio/SX1276Radio.hpp"
// #include "SIGMA.hpp"

// #include "hardware/spi.h"
// #include "hardware/gpio.h"
// #include "pico/time.h"
// #include <string.h>

// // ── Radio instance ────────────────────────────────────────────────────────────
// static const SX1276Config s_lora1_cfg {
//     LoRa1Cfg::FREQ_MHZ,
//     LoRa1Cfg::BW_KHZ,
//     LoRa1Cfg::SF,
//     LoRa1Cfg::CR,
//     LoRa1Cfg::SYNC_WORD,
//     LoRa1Cfg::TX_POWER,
//     LoRa1Cfg::PREAMBLE,
// };

// static SX1276Radio s_radio( spi0,
//                               Pins::LORA1_SCK,
//                               Pins::LORA1_MOSI,
//                               Pins::LORA1_MISO,
//                               Pins::LORA1_NSS,
//                               Pins::LORA1_DIO0,
//                               Pins::LORA1_RST,
//                               s_lora1_cfg );

// // ── Helpers ───────────────────────────────────────────────────────────────────
// static const char* state_name( FlightState s )
// {
//     switch ( s ) {
//         case FlightState::GROUND_IDLE:    return "GROUND_IDLE";
//         case FlightState::ARMED:          return "ARMED";
//         case FlightState::POWERED_ASCENT: return "POWERED_ASCENT";
//         case FlightState::COAST_ASCENT:   return "COAST_ASCENT";
//         case FlightState::APOGEE:         return "APOGEE";
//         case FlightState::DESCENT_DROGUE: return "DESCENT_DROGUE";
//         case FlightState::DESCENT_MAIN:   return "DESCENT_MAIN";
//         case FlightState::LANDED:         return "LANDED";
//         case FlightState::FAULT:          return "FAULT";
//         default:                          return "UNKNOWN";
//     }
// }

// // ── SPI diagnostic ────────────────────────────────────────────────────────────
// // Reads the SX1276 RegVersion register (0x42) via bare-metal SPI before
// // RadioLib's begin(), so the raw byte is logged regardless of outcome.
// // A second read with MISO pulled up disambiguates floating from driven-low:
// //   read=0x12, pulled=0x12 → SX1276 present and healthy
// //   read=0x00, pulled=0xFF → MISO not connected (wire missing)
// //   read=0x00, pulled=0x00 → MISO shorted to GND
// //   read=0xFF, pulled=0xFF → chip in reset / not powered
// static uint8_t diag_spi_read_reg( bool miso_pullup )
// {
//     spi_init( spi0, 1'000'000u );
//     spi_set_format( spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST );
//     gpio_set_function( Pins::LORA1_SCK,  GPIO_FUNC_SPI );
//     gpio_set_function( Pins::LORA1_MOSI, GPIO_FUNC_SPI );
//     gpio_set_function( Pins::LORA1_MISO, GPIO_FUNC_SPI );

//     if ( miso_pullup ) gpio_pull_up( Pins::LORA1_MISO );
//     else               gpio_disable_pulls( Pins::LORA1_MISO );

//     gpio_init( Pins::LORA1_NSS );
//     gpio_set_dir( Pins::LORA1_NSS, GPIO_OUT );
//     gpio_put( Pins::LORA1_NSS, 1 );

//     gpio_init( Pins::LORA1_RST );
//     gpio_set_dir( Pins::LORA1_RST, GPIO_OUT );
//     gpio_put( Pins::LORA1_RST, 0 );
//     sleep_ms( 10 );
//     gpio_put( Pins::LORA1_RST, 1 );
//     sleep_ms( 10 );

//     gpio_put( Pins::LORA1_NSS, 0 );
//     const uint8_t addr = 0x42u;
//     uint8_t val = 0xAAu;
//     spi_write_blocking( spi0, &addr, 1 );
//     spi_read_blocking(  spi0, 0x00u, &val, 1 );
//     gpio_put( Pins::LORA1_NSS, 1 );

//     gpio_disable_pulls( Pins::LORA1_MISO );
//     return val;
// }

// static void diag_spi()
// {
//     uint8_t no_pull = diag_spi_read_reg( false );
//     uint8_t pullup  = diag_spi_read_reg( true  );

//     log_print( "[lora1] SPI diag: RegVersion=0x%02X (pulled-up=0x%02X)\n",
//                no_pull, pullup );

//     if      ( no_pull == 0x12 )                    log_print( "[lora1] diag: SX1276 OK\n" );
//     else if ( no_pull == 0x00 && pullup == 0xFF )  log_print( "[lora1] diag: MISO not connected — check GPIO%u\n", Pins::LORA1_MISO );
//     else if ( no_pull == 0x00 && pullup == 0x00 )  log_print( "[lora1] diag: MISO shorted to GND\n" );
//     else if ( no_pull == 0xFF && pullup == 0xFF )  log_print( "[lora1] diag: chip in reset / no power — check GPIO%u and 3.3V\n", Pins::LORA1_RST );
//     else log_print( "[lora1] diag: unexpected — NSS=%u SCK=%u MOSI=%u MISO=%u RST=%u\n",
//                     Pins::LORA1_NSS, Pins::LORA1_SCK,
//                     Pins::LORA1_MOSI, Pins::LORA1_MISO, Pins::LORA1_RST );
// }

// // ── Task ──────────────────────────────────────────────────────────────────────
// static void lora1_task( void* )
// {
//     // Assert power-enable pin
//     gpio_init( Pins::LORA1_EN );
//     gpio_set_dir( Pins::LORA1_EN, GPIO_OUT );
//     gpio_put( Pins::LORA1_EN, 1 );

//     // Staggered start — let other tasks come up and USB log to settle
//     for ( int i = 10; i > 0; i-- ) {
//         log_print( "[lora1] init in %d s...\n", i );
//         vTaskDelay( pdMS_TO_TICKS( 1000 ) );
//     }

//     diag_spi();

//     int state = s_radio.begin();
//     if ( state != RADIO_OK ) {
//         log_print( "[lora1] init failed %d — task halting\n", state );
//         while ( true ) vTaskDelay( portMAX_DELAY );
//     }

//     log_print( "[lora1] SX1276 ready — %.1f MHz  SF%u  BW%.0f kHz  sync=0x%02X\n",
//                LoRa1Cfg::FREQ_MHZ, (unsigned)LoRa1Cfg::SF,
//                LoRa1Cfg::BW_KHZ,  (unsigned)LoRa1Cfg::SYNC_WORD );

//     log_print( "boot_ms,state,satellites,flags,"
//                "lat,lon,alt_gps_m,alt_baro_m,speed_ms,"
//                "q_w,q_x,q_y,q_z,"
//                "rssi_dBm,snr_dB\n" );

//     s_radio.startReceive();

//     for ( ;; ) {
//         if ( s_radio.packetAvailable() ) {
//             RadioPacket pkt;
//             state = s_radio.readPacket( pkt );

//             if ( state == RADIO_OK ) {
//                 SigmaLoRaData d;
//                 if ( SigmaLoRaData::deserialize(
//                          reinterpret_cast<const uint8_t*>( pkt.data ),
//                          pkt.len, d ) )
//                 {
//                     log_print( "%lu,%s,%u,%u,"
//                                "%.7f,%.7f,%.1f,%.1f,%.2f,"
//                                "%.5f,%.5f,%.5f,%.5f,"
//                                "%.1f,%.1f\n",
//                                (unsigned long)d.boot_ms,
//                                state_name( d.state ),
//                                (unsigned)d.satellites,
//                                (unsigned)d.flags,
//                                d.lat, d.lon,
//                                (double)d.alt_gps_m, (double)d.alt_baro_m,
//                                (double)d.speed_ms,
//                                (double)d.q[0], (double)d.q[1],
//                                (double)d.q[2], (double)d.q[3],
//                                (double)pkt.rssi, (double)pkt.snr );

//                     if ( mqtt_is_connected() ) {
//                         MqttMessage m;
//                         strncpy( m.topic, "rocket/lora", sizeof(m.topic) );
//                         snprintf( m.payload, sizeof(m.payload),
//                                   "{\"boot_ms\":%lu,\"state\":\"%s\","
//                                   "\"sats\":%u,\"flags\":%u,"
//                                   "\"lat\":%.7f,\"lon\":%.7f,"
//                                   "\"alt_gps_m\":%.1f,\"alt_baro_m\":%.1f,"
//                                   "\"speed_ms\":%.2f,"
//                                   "\"q\":[%.5f,%.5f,%.5f,%.5f],"
//                                   "\"rssi\":%.1f,\"snr\":%.1f}",
//                                   (unsigned long)d.boot_ms,
//                                   state_name( d.state ),
//                                   (unsigned)d.satellites,
//                                   (unsigned)d.flags,
//                                   d.lat, d.lon,
//                                   (double)d.alt_gps_m, (double)d.alt_baro_m,
//                                   (double)d.speed_ms,
//                                   (double)d.q[0], (double)d.q[1],
//                                   (double)d.q[2], (double)d.q[3],
//                                   (double)pkt.rssi, (double)pkt.snr );
//                         xQueueSend( g_mqtt_queue, &m, 0 );
//                     }

//                     if ( d.flags & SIGMA_FLAG_GPS_VALID ) {
//                         LocationMsg loc = { d.lat, d.lon, (double)d.alt_gps_m };
//                         xQueueOverwrite( g_rocket_location_q, &loc );
//                     }
//                 } else {
//                     log_print( "[lora1] rx %u B  RSSI %.1f dBm  SNR %.1f dB — bad SIGMA frame\n",
//                                (unsigned)pkt.len, (double)pkt.rssi, (double)pkt.snr );
//                 }
//             } else {
//                 log_print( "[lora1] readPacket error %d\n", state );
//             }

//             s_radio.startReceive();
//         }

//         vTaskDelay( pdMS_TO_TICKS( 10 ) );
//     }
// }

// static StaticTask_t s_lora1_tcb;
// static StackType_t  s_lora1_stack[ 2048 ];

// void lora1_task_init()
// {
//     TaskHandle_t h = xTaskCreateStatic( lora1_task, "lora1", 2048,
//                                          nullptr, tskIDLE_PRIORITY + 4,
//                                          s_lora1_stack, &s_lora1_tcb );
//     configASSERT( h );
//     vTaskCoreAffinitySet( h, 1u << 0 );
// }
