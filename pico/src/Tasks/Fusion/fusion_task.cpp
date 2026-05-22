#include "fusion_task.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

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
// AHRS runs internally at 100 Hz. Keep MQTT below that so JSON formatting and
// lwIP queueing do not become part of every filter cycle.
#define FUSION_MQTT_INTERVAL_MS 50
#define FUSION_STATUS_INTERVAL_MS 1000

// Seconds of recovery time after initialisation or disturbance (×sample rate)
#define FUSION_RECOVERY_SAMPLES ( 5 * FUSION_SAMPLE_RATE_HZ )

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

static StackType_t  s_stack[ 4096 ];
static StaticTask_t s_tcb;
static FusionAhrs   s_ahrs;
static FusionBias   s_bias;

static float wrap_360( float deg )
{
    while ( deg < 0.0f )    deg += 360.0f;
    while ( deg >= 360.0f ) deg -= 360.0f;
    return deg;
}

static void fusion_publish_status( bool have_icm, bool have_mag, uint32_t update_count )
{
    if ( !mqtt_is_connected() ) return;

    MqttMessage m = {};
    snprintf( m.topic, sizeof(m.topic), "gs/pico/primary/ahrs/status" );
    snprintf( m.payload, sizeof(m.payload),
              "{"
              "\"timestamp\":%llu,"
              "\"running\":true,"
              "\"have_imu\":%s,"
              "\"have_mag\":%s,"
              "\"updates\":%lu"
              "}",
              (unsigned long long)( time_us_64() / 1000u ),
              have_icm ? "true" : "false",
              have_mag ? "true" : "false",
              (unsigned long)update_count );
    xQueueSend( g_mqtt_queue, &m, 0 );
}

static void fusion_task( void* )
{
    // -- Wait for all sensor tasks to start and post initial readings ----------
    vTaskDelay( pdMS_TO_TICKS(500) );

    // -- AHRS initialisation ---------------------------------------------------
    FusionAhrsInitialise( &s_ahrs );
    FusionBiasInitialise( &s_bias );

    const FusionAhrsSettings ahrs_cfg = {
        .convention            = FusionConventionNed,
        .gain                  = 0.5f,
        .gyroscopeRange        = 2000.0f,
        .accelerationRejection = 10.0f,
        .magneticRejection     = 10.0f,
        .recoveryTriggerPeriod = (unsigned int)FUSION_RECOVERY_SAMPLES,
    };
    FusionAhrsSetSettings( &s_ahrs, &ahrs_cfg );

    const FusionBiasSettings bias_cfg = {
        .sampleRate          = (float)FUSION_SAMPLE_RATE_HZ,
        .stationaryThreshold = 3.0f,    // deg/s — below this = stationary
        .stationaryPeriod    = 3.0f,    // seconds stationary before correcting
    };
    FusionBiasSetSettings( &s_bias, &bias_cfg );

    log_print( "[fusion] AHRS starting at %d Hz\n", FUSION_SAMPLE_RATE_HZ );

    // -- State -----------------------------------------------------------------
    uint64_t   last_icm_ts   = 0;       // tracks when we last advanced the filter
    uint64_t   last_update_us = time_us_64();
    TickType_t last_mqtt     = xTaskGetTickCount();
    TickType_t last_status   = xTaskGetTickCount();
    TickType_t last_tick     = xTaskGetTickCount();
    uint32_t   update_count  = 0;

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
            gyro = FusionBiasUpdate( &s_bias, gyro );

            // -- 6. AHRS update ------------------------------------------------
            if ( have_mag && !FusionVectorIsZero( mag_v ) ) {
                FusionAhrsUpdate( &s_ahrs, gyro, accel, mag_v, delta_t );
            } else {
                FusionAhrsUpdateNoMagnetometer( &s_ahrs, gyro, accel, delta_t );
            }
            update_count++;
        }

        // -- 7. Extract outputs and publish to g_imu_q -------------------------
        FusionQuaternion q           = FusionAhrsGetQuaternion( &s_ahrs );
        FusionEuler      euler       = FusionQuaternionToEuler( q );
        FusionVector     earth_accel = FusionAhrsGetEarthAcceleration( &s_ahrs );
        FusionAhrsFlags  flags       = FusionAhrsGetFlags( &s_ahrs );

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
        if ( ( now_ticks - last_status ) >= pdMS_TO_TICKS(FUSION_STATUS_INTERVAL_MS) ) {
            last_status = now_ticks;
            fusion_publish_status( have_icm, have_mag, update_count );
        }

        if ( ( now_ticks - last_mqtt ) >= pdMS_TO_TICKS(FUSION_MQTT_INTERVAL_MS) ) {
            last_mqtt = now_ticks;

            if ( mqtt_is_connected() ) {
                MqttMessage m = {};
                snprintf( m.topic, sizeof(m.topic), "gs/pico/primary/imu" );
                snprintf( m.payload, sizeof(m.payload),
                          "{"
                          "\"timestamp\":%llu,"
                          "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f,\"yaw360\":%.2f,"
                          "\"q\":[%.4f,%.4f,%.4f,%.4f],"
                          "\"a\":[%.3f,%.3f,%.3f],"
                          "\"m\":[%.3f,%.3f,%.3f],"
                          "\"have_mag\":%s,"
                          "\"startup\":%s,\"mag_rec\":%s,\"acc_rec\":%s,"
                          "\"alt_baro\":%.1f,\"temp\":%.1f,"
                          "\"valid\":%s"
                          "}",
                          (unsigned long long)( time_us_64() / 1000u ),
                          (double)euler.angle.roll,
                          (double)euler.angle.pitch,
                          (double)euler.angle.yaw,
                          (double)wrap_360( euler.angle.yaw ),
                          (double)q.element.w, (double)q.element.x,
                          (double)q.element.y, (double)q.element.z,
                          (double)icm.accel[0], (double)icm.accel[1], (double)icm.accel[2],
                          (double)mag.mag[0], (double)mag.mag[1], (double)mag.mag[2],
                          have_mag ? "true" : "false",
                          flags.startup ? "true" : "false",
                          flags.magneticRecovery ? "true" : "false",
                          flags.accelerationRecovery ? "true" : "false",
                          (double)out.baro_alt_m,
                          (double)out.temp_c,
                          out.valid ? "true" : "false" );
                xQueueSend( g_mqtt_queue, &m, 0 );
            }
        }

        // -- 9. Sleep remainder of 10 ms period --------------------------------
        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(FUSION_PERIOD_MS) );
    }
}

void fusion_task_init()
{
    task_create( fusion_task, "fusion", 4096, nullptr, tskIDLE_PRIORITY + 3,
                 s_stack, &s_tcb );
}
