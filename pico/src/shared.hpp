#pragma once

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "pico/platform/panic.h"
#include <stdio.h>

// ── Task creation helper ───────────────────────────────────────────────────────
// Drop-in for xTaskCreateStatic + configASSERT.  On failure prints the task
// name, stack depth, and priority, then calls panic() to halt with a
// diagnostic blink.  Returns the handle so callers can use it (e.g. for
// vTaskCoreAffinitySet).
static inline TaskHandle_t task_create( TaskFunction_t fn,
                                         const char*    name,
                                         uint32_t       stack_depth,
                                         void*          param,
                                         UBaseType_t    priority,
                                         StackType_t*   stack,
                                         StaticTask_t*  tcb )
{
    TaskHandle_t h = xTaskCreateStatic( fn, name, stack_depth, param,
                                         priority, stack, tcb );
    if ( !h )
        panic( "[rtos] xTaskCreateStatic failed: '%s' stack=%u pri=%u",
               name, (unsigned)stack_depth, (unsigned)priority );
    return h;
}

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

// ── Pin assignments (GPIO number, Pico 2W) ────────────────────────────────────
namespace Pins {

    // ── Servo 1 (Zenith) – UART0 pins repurposed as differential step/dir ─────
    static constexpr uint SRV1_PUL_P = 0;   // GPIO 0,  phys  1  (UART0 TX)
    static constexpr uint SRV1_PUL_N = 1;   // GPIO 1,  phys  2  (UART0 RX)
    static constexpr uint SRV1_DIR_P = 2;   // GPIO 2,  phys  4  (UART0 CTS)
    static constexpr uint SRV1_DIR_N = 3;   // GPIO 3,  phys  5  (UART0 RTS)

    // ── Servo 2 (Azimuth) – UART1 pins repurposed as differential step/dir ────
    static constexpr uint SRV2_PUL_P = 4;   // GPIO 4,  phys  6  (UART1 TX)
    static constexpr uint SRV2_PUL_N = 5;   // GPIO 5,  phys  7  (UART1 RX)
    static constexpr uint SRV2_DIR_P = 6;   // GPIO 6,  phys  9  (UART1 CTS)
    static constexpr uint SRV2_DIR_N = 7;   // GPIO 7,  phys 10  (UART1 RTS)

    // ── LoRa 1 (SX1276 / 915 MHz) – SPI0 ────────────────────────────────────
    static constexpr uint LORA1_RST  =  8;  // GPIO 8,  phys 11
    static constexpr uint LORA1_DIO0 =  9;  // GPIO 9,  phys 12  (G0 / IRQ)
    static constexpr uint LORA1_EN   = 10;  // GPIO 10, phys 14  (power enable)
    static constexpr uint LORA1_SCK  = 18;  // GPIO 18, phys 24
    static constexpr uint LORA1_MOSI = 19;  // GPIO 19, phys 25
    static constexpr uint LORA1_MISO = 20;  // GPIO 20, phys 26
    static constexpr uint LORA1_NSS  = 21;  // GPIO 21, phys 27  (CS)

    // ── LoRa 2 (RFM69HCW / 433 MHz) – SPI1 ──────────────────────────────────
    static constexpr uint LORA2_RST  = 11;  // GPIO 11, phys 15
    static constexpr uint LORA2_MISO = 12;  // GPIO 12, phys 16
    static constexpr uint LORA2_NSS  = 13;  // GPIO 13, phys 17  (CS)
    static constexpr uint LORA2_SCK  = 14;  // GPIO 14, phys 19
    static constexpr uint LORA2_MOSI = 15;  // GPIO 15, phys 20
    static constexpr uint LORA2_DIO0 = 22;  // GPIO 22, phys 29  (G0 / IRQ)
    static constexpr uint LORA2_EN   = 28;  // GPIO 28, phys 34  (power enable)

    // ── I2C0 (not populated – Barometer) ────────────────────────────────────
    static constexpr uint I2C0_SDA   = 16;  // GPIO 16, phys 21
    static constexpr uint I2C0_SCL   = 17;  // GPIO 17, phys 22

    // ── I2C1 (not populated – IMU / Magnetometer) ────────────────────────────
    static constexpr uint I2C1_SDA   = 26;  // GPIO 26, phys 31
    static constexpr uint I2C1_SCL   = 27;  // GPIO 27, phys 32

} // namespace Pins

// ── LoRa radio parameters ─────────────────────────────────────────────────────
namespace LoRa1Cfg {
    static constexpr float    FREQ_MHZ   = 915.0f;
    static constexpr float    BW_KHZ     = 125.0f;
    static constexpr uint8_t  SF         = 7;
    static constexpr uint8_t  CR         = 5;
    static constexpr uint8_t  SYNC_WORD  = 0x12;
    static constexpr int8_t   TX_POWER   = 20;
    static constexpr uint16_t PREAMBLE   = 8;
}

namespace LoRa2Cfg {
    static constexpr float   FREQ_MHZ    = 433.0f;
    static constexpr float   BR_KBPS     = 4.8f;
    static constexpr float   FREQ_DEV_KHZ = 5.0f;
    static constexpr float   RX_BW_KHZ   = 125.0f;
    static constexpr int8_t  TX_POWER    = 20;    // RFM69HCW high-power mode
    static constexpr uint8_t PREAMBLE    = 16;
}

// ── Shared location queues ─────────────────────────────────────────────────────
// Depth-1 overwrite queues carrying the latest known positions.
// Writers use xQueueOverwrite(); readers use xQueuePeek().
//
// g_gs_location_q     – written by gps_task when ground station position is known
//                       (Starlink WiFi → MQTT subscription, see gps_task.cpp).
// g_rocket_location_q – written by lora1_task when a GPS-valid SIGMA frame arrives.
struct LocationMsg {
    double lat;
    double lon;
    double alt_m;
};

extern QueueHandle_t g_gs_location_q;
extern QueueHandle_t g_rocket_location_q;

// ── WiFi / MQTT configuration ──────────────────────────────────────────────────
#define WIFI_SSID         "ssid"     // change to your Starlink SSID
#define WIFI_PASSWORD     "password"   // change to your Starlink password
#define MQTT_BROKER_HOST   "192.168.8.171"
#define MQTT_BROKER_PORT   1883
#define MQTT_CLIENT_ID     "gs_pico"
