#pragma once

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "pico/platform/panic.h"
#include <stdio.h>

// -- Task creation helper -------------------------------------------------------
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

// -- Log queue -----------------------------------------------------------------
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

// -- Network event bits --------------------------------------------------------
#define EVT_WIFI_CONNECTED   ( 1 << 0 )

extern EventGroupHandle_t g_net_events;

// -- MQTT publish queue --------------------------------------------------------
// All tasks publish by enqueuing an MqttMessage.  The MQTT task drains it.
struct MqttMessage {
    char     topic[ 64 ];
    uint16_t payload_len;
    uint8_t  payload[ 320 ];
};

#define MQTT_QUEUE_DEPTH  64
extern QueueHandle_t g_mqtt_queue;

// -- Pin assignments -----------------------------------------------------------
// Single source of truth for the Ground Station PCB rev 1 (Pico 2 W) lives in
// the shared board pin map. LORA0 = 915 MHz LoRa (SX1276, SPI0); LORA1 = 433 MHz
// GFSK (RFM69, SPI1); GPS on UART0 (TX=12, RX=13).
#include "boards/gs_pcb_v1_pins.hpp"

// -- LoRa radio parameters -----------------------------------------------------
namespace LoRa0Cfg {
    static constexpr float    FREQ_MHZ   = 915.0f;
    static constexpr float    BW_KHZ     = 125.0f;
    static constexpr uint8_t  SF         = 7;
    static constexpr uint8_t  CR         = 5;
    static constexpr uint8_t  SYNC_WORD  = 0x12;
    static constexpr int8_t   TX_POWER   = 20;
    static constexpr uint16_t PREAMBLE   = 8;
}

namespace LoRa1Cfg {
    static constexpr float   FREQ_MHZ    = 433.0f;
    static constexpr float   BR_KBPS     = 4.8f;
    static constexpr float   FREQ_DEV_KHZ = 5.0f;
    static constexpr float   RX_BW_KHZ   = 125.0f;
    static constexpr int8_t  TX_POWER    = 20;    // RFM69HCW high-power mode
    static constexpr uint8_t PREAMBLE    = 16;
}

namespace LoRa2Cfg = LoRa1Cfg;

// -- Shared location queues -----------------------------------------------------
// Depth-1 overwrite queues carrying the latest known positions.
// Writers use xQueueOverwrite(); readers use xQueuePeek().
//
// g_gs_location_q     – written by gps_task when ground station position is known
//                       (Starlink WiFi -> MQTT subscription, see gps_task.cpp).
// g_rocket_location_q – written by lora0_task when a GPS-valid SIGMA frame arrives.
struct LocationMsg {
    double lat;
    double lon;
    double alt_m;
    uint64_t timestamp_us;
};

extern QueueHandle_t g_gs_location_q;
extern QueueHandle_t g_rocket_location_q;

// -- Raw sensor queues (depth-1 overwrite) ------------------------------------
// Each sensor task writes its latest reading.  fusion_task peeks all three.

struct IcmMsg {
    float    accel[3];      // g           (±16 g range)
    float    gyro[3];       // deg/s       (±2000 dps range)
    float    temp_c;        // die temperature from the IMU
    uint64_t timestamp_us;  // time_us_64() at time of read
    bool     valid;
};

struct MagMsg {
    float    mag[3];        // gauss       (±4 G range)
    uint64_t timestamp_us;
    bool     valid;
};

struct YawImuMsg {
    float    accel[3];      // g           (LSM6DSOX on yaw moving body)
    float    gyro[3];       // deg/s
    float    mag_ut[3];     // microtesla  (LIS3MDL on yaw moving body)
    float    temp_c;
    uint64_t timestamp_us;
    bool     mag_valid;
    bool     mag_overflow;
    bool     valid;
};

struct BaroMsg {
    float    pressure_pa;   // calibrated pressure, Pa
    float    alt_m;         // ISA altitude, metres MSL
    float    temp_c;        // calibrated temperature, °C
    uint64_t timestamp_us;
    bool     valid;         // false until first complete D1+D2 pair
};

extern QueueHandle_t g_icm_q;
extern QueueHandle_t g_mag_q;
extern QueueHandle_t g_yaw_imu_q;
extern QueueHandle_t g_baro_q;

// -- IMU / AHRS output queue ---------------------------------------------------
// Written by fusion_task at 100 Hz (depth-1 overwrite).
// Readers call xQueuePeek() to get the latest orientation without consuming it.
// `valid` is false until the Fusion AHRS startup ramp has completed (~3 s).
struct ImuMsg {
    uint64_t timestamp_us;
    float q[4];             // quaternion [w, x, y, z] sensor->Earth (NED)
    float euler[3];         // [roll_deg, pitch_deg, yaw_deg] ZYX convention
    float earth_accel[3];   // linear acceleration, Earth frame, g (gravity removed)
    float baro_alt_m;       // barometric altitude, metres MSL
    float temp_c;           // temperature (baro sensor preferred, else IMU)
    bool  valid;            // true once AHRS startup ramp is done
    bool  have_yaw_frame;
    float yaw_frame_yaw360;
    float bar_rel_pitch;
};

extern QueueHandle_t g_imu_q;

// -- WiFi / MQTT configuration --------------------------------------------------
#define WIFI_SSID         "RocketryAtVT"     // change to your Starlink SSID
#define WIFI_PASSWORD     "RocketryAtVT147"   // change to your Starlink password
#define MQTT_BROKER_HOST   "192.168.1.170"
#define MQTT_BROKER_PORT   1883
#define MQTT_CLIENT_ID     "gs_pico"

// -- Inter-Pico UDP configuration -----------------------------------------------
// The secondary Pico sends SIGMA INTER_PICO frames to this port on the primary.
// Assign the primary Pico a static IP via the Starlink AP's DHCP reservation.
#define INTER_PICO_PORT    5005
