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
// Single source of truth for the active board pins and this firmware's plugged-in
// devices. LORA0 = 915 MHz LoRa (SX1276, SPI0); LORA1 = 433 MHz GFSK (RFM69,
// SPI1); GPS on UART0 (TX=12, RX=13).
#include "boards/board.hpp"

static_assert(HAS_GPS, "gs_pico requires a GPS in board_profile.hpp");
static_assert(HAS_RADIO, "gs_pico requires radios in board_profile.hpp");
static_assert(Board::RadioCount >= 2, "gs_pico requires LORA0 and LORA1 radio profiles");
static_assert(Board::GpsCount > 0, "gs_pico requires at least one GPS profile");
static_assert(Board::Radios[0].model == Board::RadioModel::SX1276,
              "Board::Radios[0] must be the LORA0 SX1276");
static_assert(Board::Radios[0].bus == Board::Bus::SPI0,
              "LORA0 SX1276 must be on SPI0");
static_assert(Board::Radios[1].model == Board::RadioModel::RFM69HCW,
              "Board::Radios[1] must be the LORA1 RFM69HCW");
static_assert(Board::Radios[1].bus == Board::Bus::SPI1,
              "LORA1 RFM69HCW must be on SPI1");
static_assert(Board::Gpses[0].bus == Board::Bus::UART0,
              "ground-station GPS task currently supports UART0 only");
static_assert(Board::Gpses[0].nav_hz > 0, "GPS nav_hz must be non-zero");
static_assert(Board::Gpses[0].nav_hz <= Board::spec_of(Board::Gpses[0].model).max_nav_hz,
              "GPS nav_hz exceeds selected receiver spec");
static_assert(Board::Radios[0].freq_mhz >= Board::spec_of(Board::Radios[0].model).freq_min_mhz &&
              Board::Radios[0].freq_mhz <= Board::spec_of(Board::Radios[0].model).freq_max_mhz,
              "LORA0 frequency outside selected radio spec");
static_assert(Board::Radios[1].freq_mhz >= Board::spec_of(Board::Radios[1].model).freq_min_mhz &&
              Board::Radios[1].freq_mhz <= Board::spec_of(Board::Radios[1].model).freq_max_mhz,
              "LORA1 frequency outside selected radio spec");

// -- LoRa radio parameters -----------------------------------------------------
namespace LoRa0Cfg {
    static constexpr float    FREQ_MHZ   = Board::Radios[0].freq_mhz;
    static constexpr float    BW_KHZ     = Board::Lora915::BW_KHZ;
    static constexpr uint8_t  SF         = Board::Lora915::SF;
    static constexpr uint8_t  CR         = Board::Lora915::CR;
    static constexpr uint8_t  SYNC_WORD  = Board::Lora915::SYNC_WORD;
    static constexpr int8_t   TX_POWER   = Board::Lora915::TX_DBM;
    static constexpr uint16_t PREAMBLE   = Board::Lora915::PREAMBLE;
}

namespace LoRa1Cfg {
    static constexpr float    FREQ_MHZ     = Board::Radios[1].freq_mhz;
    static constexpr float    BR_KBPS      = Board::Rfm433::BR_KBPS;
    static constexpr float    FREQ_DEV_KHZ = Board::Rfm433::FDEV_KHZ;
    static constexpr float    RX_BW_KHZ    = Board::Rfm433::RXBW_KHZ;
    static constexpr int8_t   TX_POWER     = Board::Rfm433::TX_DBM;
    static constexpr uint16_t PREAMBLE     = Board::Rfm433::PREAMBLE;
    static constexpr bool     HIGH_POWER   = Board::Rfm433::HIGH_POWER;
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
    float vel_n_mps;
    float vel_e_mps;
    float vel_d_mps;
    float h_acc_m;
    float v_acc_m;
    float s_acc_mps;
    uint32_t source_boot_ms;
    bool have_velocity;
    bool have_accuracy;
};

extern QueueHandle_t g_gs_location_q;
extern QueueHandle_t g_rocket_location_q;

// -- Altitude-only tracking ----------------------------------------------------
// Used when only a barometric altitude feed is available. `alt_m` is MSL.
struct AltitudeMsg {
    float alt_m;
    uint32_t source_boot_ms;
    uint64_t timestamp_us;
    float rssi;
    float snr;
    bool valid;
};

extern QueueHandle_t g_rocket_altitude_q;

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
#define WIFI_PASSWORD     "RocketryAtVT52"   // change to your Starlink password
#define MQTT_BROKER_HOST   "192.168.88.252"
#define MQTT_BROKER_PORT   1883
#define MQTT_CLIENT_ID     "gs_pico"

// -- Inter-Pico UDP configuration -----------------------------------------------
// The secondary Pico sends SIGMA INTER_PICO frames to this port on the primary.
// Assign the primary Pico a static IP via the Starlink AP's DHCP reservation.
#define INTER_PICO_PORT    5005
