#pragma once

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"

// ── Log queue ─────────────────────────────────────────────────────────────────
// Tasks call log_print() instead of printf().  The USB task is the sole
// consumer and the only code that calls printf(), avoiding stdio contention.
//
// IMPORTANT: log_print() uses xQueueSend — do NOT call it from ISR context.
struct LogMessage {
    char buf[ 256 ];
};

#define LOG_QUEUE_DEPTH  64
extern QueueHandle_t g_log_queue;

// Printf-style logger — safe from any FreeRTOS task, never from ISR.
void log_print( const char* fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));

// ── Network event bits ────────────────────────────────────────────────────────
#define EVT_WIFI_CONNECTED   ( 1 << 0 )

extern EventGroupHandle_t g_net_events;

// ── MQTT publish queue ────────────────────────────────────────────────────────
// All tasks publish by enqueuing an MqttMessage.  The MQTT task drains it.
struct MqttMessage {
    char topic  [ 64 ];
    char payload[ 384 ];
};

#define MQTT_QUEUE_DEPTH  8
extern QueueHandle_t g_mqtt_queue;

// ── Pin assignments (physical pin → GPIO, Pico 2W) ───────────────────────────
namespace Pins {
    // Servos
    static constexpr uint SERVO_ZENITH   =  0;   // Physical pin 1
    static constexpr uint SERVO_AZIMUTH  =  1;   // Physical pin 2

    // GPS – UART1
    static constexpr uint GPS_UART_TX    =  8;   // Physical pin 11 → GPS RX
    static constexpr uint GPS_UART_RX    =  9;   // Physical pin 12 ← GPS TX (we read this)

    // LoRa – SX1276 via SPI0
    static constexpr uint LORA_RST       = 10;   // Physical pin 14
    static constexpr uint LORA_DIO0      = 11;   // Physical pin 15  (G0 / packet-ready)
    static constexpr uint LORA_SCK       = 18;   // Physical pin 24  SPI0 SCK
    static constexpr uint LORA_MOSI      = 19;   // Physical pin 25  SPI0 TX  → LoRa MOSI
    static constexpr uint LORA_MISO      = 20;   // Physical pin 26  SPI0 RX  ← LoRa MISO
    static constexpr uint LORA_NSS       = 21;   // Physical pin 27  SPI0 CSn
}

// ── LoRa radio parameters (must match transmitter) ───────────────────────────
namespace LoRaCfg {
    static constexpr float    FREQ_MHZ   = 915.0f;
    static constexpr float    BW_KHZ     = 125.0f;
    static constexpr uint8_t  SF         = 7;
    static constexpr uint8_t  CR         = 5;
    static constexpr uint8_t  SYNC_WORD  = 0x12;
    static constexpr int8_t   TX_POWER   = 20;
    static constexpr uint16_t PREAMBLE   = 8;
}

// ── Shared location queues ────────────────────────────────────────────────────
// Depth-1 overwrite queues carrying the latest known positions.
// Writers use xQueueOverwrite(); readers use xQueuePeek().
//
// g_gs_location_q   — written by GPS task when a fix is held.
// g_rocket_location_q — written by LoRa task once the rocket data format is
//                       defined (stub TODO in lora_task.cpp).
struct LocationMsg {
    double lat;
    double lon;
    double alt_m;
};

extern QueueHandle_t g_gs_location_q;
extern QueueHandle_t g_rocket_location_q;

// ── WiFi / MQTT configuration ─────────────────────────────────────────────────
// WIFI_SSID / WIFI_PASSWORD are injected as compile definitions from CMake.
#define MQTT_BROKER_HOST   "192.168.1.100"   // change to your broker's IP
#define MQTT_BROKER_PORT   1883
#define MQTT_CLIENT_ID     "gs_pico"
