#include "fusion_task.hpp"
#include "shared.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include "Fusion.h"

#include <string.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
// Sensor calibration stubs
// Replace with values from your calibration procedure.
// -----------------------------------------------------------------------------

// Gyroscope misalignment, sensitivity error, bias offset
static const FusionMatrix k_gyro_misalign  = {1,0,0, 0,1,0, 0,0,1};
static const FusionVector k_gyro_sens      = {1.0f, 1.0f, 1.0f};
static const FusionVector k_gyro_offset    = {0.0f, 0.0f, 0.0f};

// Accelerometer misalignment, sensitivity error, bias offset
static const FusionMatrix k_accel_misalign = {1,0,0, 0,1,0, 0,0,1};
static const FusionVector k_accel_sens     = {1.0f, 1.0f, 1.0f};
static const FusionVector k_accel_offset   = {0.0f, 0.0f, 0.0f};

// Magnetometer soft-iron matrix and hard-iron offset
static const FusionMatrix k_mag_soft_iron  = {1,0,0, 0,1,0, 0,0,1};
static const FusionVector k_mag_hard_iron  = {0.0f, 0.0f, 0.0f};

// -----------------------------------------------------------------------------
// Axis remapping
// Change to match the physical orientation of the sensors on the ground station.
// FusionRemapAlignmentPXPYPZ = identity (no remapping).
// -----------------------------------------------------------------------------

#define SENSOR_REMAP  FusionRemapAlignmentPXPYPZ

// -----------------------------------------------------------------------------
// Fusion AHRS settings
// -----------------------------------------------------------------------------

#define FUSION_SAMPLE_RATE_HZ   100
#define FUSION_PERIOD_MS        ( 1000 / FUSION_SAMPLE_RATE_HZ )    // 10 ms
#define FUSION_MQTT_INTERVAL_MS 1000

// Seconds of recovery time after initialisation or disturbance (×sample rate)
#define FUSION_RECOVERY_SAMPLES ( 5 * FUSION_SAMPLE_RATE_HZ )

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

static StackType_t  s_stack[ 1536 ];
static StaticTask_t s_tcb;

static void fusion_task( void* )
{
    // -- Wait for all sensor tasks to start and post initial readings ----------
    vTaskDelay( pdMS_TO_TICKS(500) );

    // -- AHRS initialisation ---------------------------------------------------
    FusionAhrs ahrs;
    FusionBias bias;

    FusionAhrsInitialise( &ahrs );
    FusionBiasInitialise( &bias );

    const FusionAhrsSettings ahrs_cfg = {
        .convention            = FusionConventionNed,
        .gain                  = 0.5f,
        .gyroscopeRange        = 2000.0f,
        .accelerationRejection = 10.0f,
        .magneticRejection     = 10.0f,
        .recoveryTriggerPeriod = (unsigned int)FUSION_RECOVERY_SAMPLES,
    };
    FusionAhrsSetSettings( &ahrs, &ahrs_cfg );

    const FusionBiasSettings bias_cfg = {
        .sampleRate          = (float)FUSION_SAMPLE_RATE_HZ,
        .stationaryThreshold = 3.0f,    // deg/s — below this = stationary
        .stationaryPeriod    = 3.0f,    // seconds stationary before correcting
    };
    FusionBiasSetSettings( &bias, &bias_cfg );

    log_print( "[fusion] AHRS starting at %d Hz\n", FUSION_SAMPLE_RATE_HZ );

    // -- State -----------------------------------------------------------------
    uint64_t   last_icm_ts   = 0;       // tracks when we last advanced the filter
    uint64_t   last_update_us = time_us_64();
    TickType_t last_mqtt     = xTaskGetTickCount();
    TickType_t last_tick     = xTaskGetTickCount();

    for ( ;; ) {
        // -- 1. Peek latest sensor readings ------------------------------------
        IcmMsg  icm  = {};
        MagMsg  mag  = {};
        BaroMsg baro = {};

        const bool have_icm  = ( xQueuePeek( g_icm_q,  &icm,  0 ) == pdTRUE ) && icm.valid;
        const bool have_mag  = ( xQueuePeek( g_mag_q,  &mag,  0 ) == pdTRUE ) && mag.valid;
        const bool have_baro = ( xQueuePeek( g_baro_q, &baro, 0 ) == pdTRUE ) && baro.valid;

        // -- 2. Only advance filter when fresh ICM data is available -----------
        if ( have_icm && icm.timestamp_us != last_icm_ts ) {

            uint64_t now_us = time_us_64();
            float delta_t   = (float)( now_us - last_update_us ) * 1e-6f;
            // Clamp to a sane range — prevents wild jumps after pauses/debugger
            if ( delta_t < 0.001f || delta_t > 0.1f )
                delta_t = FUSION_PERIOD_MS / 1000.0f;
            last_update_us = now_us;
            last_icm_ts    = icm.timestamp_us;

            // -- 3. Apply calibration models -----------------------------------
            FusionVector gyro  = {icm.gyro[0],  icm.gyro[1],  icm.gyro[2]};
            FusionVector accel = {icm.accel[0], icm.accel[1], icm.accel[2]};
            FusionVector mag_v = {mag.mag[0],   mag.mag[1],   mag.mag[2]};

            gyro  = FusionModelInertial( gyro,  k_gyro_misalign,  k_gyro_sens,  k_gyro_offset  );
            accel = FusionModelInertial( accel, k_accel_misalign, k_accel_sens, k_accel_offset );
            mag_v = FusionModelMagnetic( mag_v, k_mag_soft_iron,  k_mag_hard_iron );

            // -- 4. Remap axes to body / NED frame -----------------------------
            gyro  = FusionRemap( gyro,  SENSOR_REMAP );
            accel = FusionRemap( accel, SENSOR_REMAP );
            mag_v = FusionRemap( mag_v, SENSOR_REMAP );

            // -- 5. Gyro bias estimation ----------------------------------------
            gyro = FusionBiasUpdate( &bias, gyro );

            // -- 6. AHRS update ------------------------------------------------
            if ( have_mag && !FusionVectorIsZero( mag_v ) ) {
                FusionAhrsUpdate( &ahrs, gyro, accel, mag_v, delta_t );
            } else {
                FusionAhrsUpdateNoMagnetometer( &ahrs, gyro, accel, delta_t );
            }
        }

        // -- 7. Extract outputs and publish to g_imu_q -------------------------
        FusionQuaternion q           = FusionAhrsGetQuaternion( &ahrs );
        FusionEuler      euler       = FusionQuaternionToEuler( q );
        FusionVector     earth_accel = FusionAhrsGetEarthAcceleration( &ahrs );
        FusionAhrsFlags  flags       = FusionAhrsGetFlags( &ahrs );

        ImuMsg out = {};
        out.q[0]           = q.element.w;
        out.q[1]           = q.element.x;
        out.q[2]           = q.element.y;
        out.q[3]           = q.element.z;
        out.euler[0]       = euler.angle.roll;
        out.euler[1]       = euler.angle.pitch;
        out.euler[2]       = euler.angle.yaw;
        out.earth_accel[0] = earth_accel.axis.x;
        out.earth_accel[1] = earth_accel.axis.y;
        out.earth_accel[2] = earth_accel.axis.z;
        out.baro_alt_m     = have_baro ? baro.alt_m : 0.0f;
        out.temp_c         = have_baro ? baro.temp_c
                                       : ( have_icm ? icm.temp_c : 25.0f );
        out.valid          = !flags.startup;

        xQueueOverwrite( g_imu_q, &out );

        // -- 8. MQTT publish (~1 Hz) -------------------------------------------
        TickType_t now_ticks = xTaskGetTickCount();
        if ( ( now_ticks - last_mqtt ) >= pdMS_TO_TICKS(FUSION_MQTT_INTERVAL_MS) ) {
            last_mqtt = now_ticks;

            MqttMessage m = {};
            snprintf( m.topic, sizeof(m.topic), "gs/pico/primary/imu" );
            snprintf( m.payload, sizeof(m.payload),
                      "{"
                      "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f,"
                      "\"q\":[%.4f,%.4f,%.4f,%.4f],"
                      "\"alt_baro\":%.1f,\"temp\":%.1f,"
                      "\"valid\":%s"
                      "}",
                      (double)euler.angle.roll,
                      (double)euler.angle.pitch,
                      (double)euler.angle.yaw,
                      (double)q.element.w, (double)q.element.x,
                      (double)q.element.y, (double)q.element.z,
                      (double)out.baro_alt_m,
                      (double)out.temp_c,
                      out.valid ? "true" : "false" );
            xQueueSend( g_mqtt_queue, &m, 0 );
        }

        // -- 9. Sleep remainder of 10 ms period --------------------------------
        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(FUSION_PERIOD_MS) );
    }
}

void fusion_task_init()
{
    task_create( fusion_task, "fusion", 1536, nullptr, tskIDLE_PRIORITY + 3,
                 s_stack, &s_tcb );
}
