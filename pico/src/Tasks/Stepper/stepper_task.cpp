#include "stepper_task.hpp"
#include "cl57te.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include "math_utils/GroundStationMath.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
// Motor / drive configuration
//
// CL57TE DIP-switch: 1600 pulses/rev (SW1=ON, SW2=OFF, SW3=ON, SW4=ON).
// Total gear reduction = 10:1 planetary gearbox × belt ratio per axis.
// -----------------------------------------------------------------------------

namespace StepCfg {
    static constexpr uint32_t PULSES_PER_REV       = 1600;

    // Per-axis gear ratios  (10:1 planetary gearbox × belt ratio)
    static constexpr float    AZ_GEAR_RATIO         = 30.0f;   // 10:1 × 3:1 belt  (azimuth)
    static constexpr float    ZEN_GEAR_RATIO        = 50.0f;   // 10:1 × 5:1 belt  (zenith)

    static constexpr float    MAX_SPEED_DPS         = 90.0f;
    static constexpr float    DEFAULT_SPEED_DPS     = 30.0f;

    // Azimuth: shortest-path rotation, no cable-wrap limit yet
    static constexpr float    AZ_MIN_DEG            = -180.0f;
    static constexpr float    AZ_MAX_DEG            =  180.0f;

    // Zenith: mechanical travel limits
    static constexpr float    ZEN_MIN_DEG           =   0.0f;
    static constexpr float    ZEN_MAX_DEG           =  90.0f;

    static constexpr uint32_t STATE_PUBLISH_MS      = 1000;
}

static float wrap_360( float deg )
{
    while ( deg < 0.0f )    deg += 360.0f;
    while ( deg >= 360.0f ) deg -= 360.0f;
    return deg;
}

// -----------------------------------------------------------------------------
// Queue handles
// -----------------------------------------------------------------------------

QueueHandle_t g_stepper_az_cmd_q    = nullptr;
QueueHandle_t g_stepper_zen_cmd_q   = nullptr;
QueueHandle_t g_stepper_az_status_q = nullptr;
QueueHandle_t g_stepper_zen_status_q = nullptr;

// -----------------------------------------------------------------------------
// Per-axis hardware task context
// -----------------------------------------------------------------------------

struct AxisCfg {
    Cl57te::Config drv;         // hardware pin + motor config
    QueueHandle_t* cmd_q;       // pointer to command queue handle
    QueueHandle_t* status_q;    // pointer to status queue handle
    float min_deg;              // soft travel limit (lower)
    float max_deg;              // soft travel limit (upper)
    const char*    tag;
};

struct AxisContext {
    Cl57te   drv;
    AxisCfg  cfg;
};

static void axis_task_body( void* arg )
{
    auto* ctx = static_cast<AxisContext*>( arg );
    Cl57te&       drv = ctx->drv;
    const AxisCfg& ax = ctx->cfg;

    // Initialise hardware
    drv.init( ax.drv );

    vTaskDelay( pdMS_TO_TICKS(200) );  // CL57TE startup settling

    log_print( "[%s] ready  steps/deg=%.2f\n",
               ax.tag, drv.steps_per_deg() );

    int32_t  pos_steps    = 0;   // commanded position (what we told the drive)
    int32_t  target_steps = 0;   // latest target from command queue
    bool     move_pending_completion = false;

    for ( ;; ) {
        // -- Process latest command -------------------------------------------
        StepperCmd cmd = {};
        if ( xQueuePeek( *ax.cmd_q, &cmd, 0 ) == pdTRUE ) {

            if ( cmd.stop ) {
                drv.stop();
                pos_steps = drv.pos_steps();
                move_pending_completion = false;

            } else {
                float clamped = cmd.target_angle_deg;
                if ( clamped < ax.min_deg ) clamped = ax.min_deg;
                if ( clamped > ax.max_deg ) clamped = ax.max_deg;

                int32_t new_target = static_cast<int32_t>(
                    roundf( clamped * drv.steps_per_deg() ) );

                if ( new_target != target_steps ) {
                    target_steps = new_target;
                    if ( drv.is_moving() ) {
                        drv.stop();
                        pos_steps = drv.pos_steps();
                        move_pending_completion = false;
                        log_print( "[%s] move interrupted for new target_steps=%ld\n",
                                   ax.tag,
                                   (long)target_steps );
                    }
                }
            }
        }

        // -- Issue move if needed ---------------------------------------------
        if ( !cmd.stop && !drv.is_moving() ) {
            if ( move_pending_completion ) {
                pos_steps = drv.pos_steps();
                move_pending_completion = false;
                log_print( "[%s] move complete pos_steps=%ld angle=%.2f\n",
                           ax.tag,
                           (long)pos_steps,
                           (double)drv.angle_deg() );
            }

            pos_steps = drv.pos_steps();
            int32_t delta = target_steps - pos_steps;
            if ( delta != 0 ) {
                StepperCmd c = {};
                xQueuePeek( *ax.cmd_q, &c, 0 );
                uint32_t hz = ( c.speed_dps > 0.0f )
                    ? static_cast<uint32_t>( c.speed_dps * drv.steps_per_deg() )
                    : 0;

                if ( drv.start_move( delta, hz ) ) {
                    move_pending_completion = true;
                    log_print( "[%s] move delta_steps=%ld target_steps=%ld speed_hz=%lu dir_gpio=%u dir_level=%u dir_readback=%u\n",
                               ax.tag,
                               (long)delta,
                               (long)target_steps,
                               (unsigned long)hz,
                               (unsigned)drv.dir_pin(),
                               drv.dir_level_for_steps( delta ) ? 1u : 0u,
                               drv.dir_gpio_level() ? 1u : 0u );
                }
            }
        }

        // -- Publish status ---------------------------------------------------
        StepperStatus st = {};
        st.angle_deg    = drv.angle_deg();
        st.moving       = drv.is_moving();
        st.faulted      = false;
        st.enabled      = true;
        st.timestamp_us = time_us_64();
        xQueueOverwrite( *ax.status_q, &st );

        vTaskDelay( pdMS_TO_TICKS(10) );    // 100 Hz poll rate
    }
}

// -----------------------------------------------------------------------------
// Stepper controller task
// Reads ground-station and rocket location queues, computes az/zenith, posts cmds.
// -----------------------------------------------------------------------------

static void stepper_ctrl_task( void* )
{
    log_print( "[step_ctrl] waiting for location fixes...\n" );

    LocationMsg gs  = {};
    LocationMsg rkt = {};
    bool have_gs    = false;
    bool have_rkt   = false;

    for ( ;; ) {
        if ( g_gs_location_q     && xQueuePeek( g_gs_location_q,     &gs,  0 ) == pdTRUE ) have_gs  = true;
        if ( g_rocket_location_q && xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE ) have_rkt = true;

        if ( have_gs && have_rkt ) {
            rocket_math::Location station{ gs.lat,  gs.lon,  gs.alt_m  };
            rocket_math::Location rocket { rkt.lat, rkt.lon, rkt.alt_m };

            double az  = rocket_math::GroundStationMath::calculateAzimuth(  station, rocket );
            double zen = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            // Normalise azimuth to [-180, 180] for shortest-path rotation
            while ( az >  180.0 ) az -= 360.0;
            while ( az < -180.0 ) az += 360.0;

            StepperCmd az_cmd  { static_cast<float>( az  ), 0.0f, false };
            StepperCmd zen_cmd { static_cast<float>( zen ), 0.0f, false };

            if ( g_stepper_az_cmd_q  ) xQueueOverwrite( g_stepper_az_cmd_q,  &az_cmd  );
            if ( g_stepper_zen_cmd_q ) xQueueOverwrite( g_stepper_zen_cmd_q, &zen_cmd );
        }

        vTaskDelay( pdMS_TO_TICKS(100) );   // 10 Hz tracking loop
    }
}

// -----------------------------------------------------------------------------
// Queue backing storage
// -----------------------------------------------------------------------------

static StaticQueue_t s_az_cmd_buf,    s_zen_cmd_buf;
static StaticQueue_t s_az_status_buf, s_zen_status_buf;

static uint8_t s_az_cmd_storage   [ sizeof(StepperCmd)    ];
static uint8_t s_zen_cmd_storage  [ sizeof(StepperCmd)    ];
static uint8_t s_az_status_storage[ sizeof(StepperStatus) ];
static uint8_t s_zen_status_storage[ sizeof(StepperStatus) ];

// -----------------------------------------------------------------------------
// Azimuth axis  (STEP1 – PUL- GPIO 4, DIR- GPIO 5)
// 10:1 planetary gearbox × 3:1 belt = 30:1 total, travel ±180°
// -----------------------------------------------------------------------------

static AxisContext s_az_ctx;

static StaticTask_t s_az_tcb;
static StackType_t  s_az_stack[ 512 ];

void stepper_az_task_init()
{
    g_stepper_az_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_az_cmd_storage, &s_az_cmd_buf );
    g_stepper_az_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_az_status_storage, &s_az_status_buf );

    s_az_ctx.cfg = AxisCfg {
        .drv = {
            .pul_n           = Pins::STEP1_PUL_N,
            .dir_n           = Pins::STEP1_DIR_N,
            .pulses_per_rev  = StepCfg::PULSES_PER_REV,
            .gear_ratio      = StepCfg::AZ_GEAR_RATIO,
            .max_speed_dps   = StepCfg::MAX_SPEED_DPS,
            .default_speed_dps = StepCfg::DEFAULT_SPEED_DPS,
        },
        .cmd_q    = &g_stepper_az_cmd_q,
        .status_q = &g_stepper_az_status_q,
        .min_deg  = StepCfg::AZ_MIN_DEG,
        .max_deg  = StepCfg::AZ_MAX_DEG,
        .tag      = "az",
    };

    task_create( axis_task_body, "step_az", 512, &s_az_ctx,
                 tskIDLE_PRIORITY + 3, s_az_stack, &s_az_tcb );
}

// -----------------------------------------------------------------------------
// Zenith axis  (STEP2 – PUL- GPIO 6, DIR- GPIO 7)
// 10:1 planetary gearbox × 5:1 belt = 50:1 total, travel 0–90°
// -----------------------------------------------------------------------------

static AxisContext s_zen_ctx;

static StaticTask_t s_zen_tcb;
static StackType_t  s_zen_stack[ 512 ];

void stepper_zen_task_init()
{
    g_stepper_zen_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_zen_cmd_storage, &s_zen_cmd_buf );
    g_stepper_zen_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_zen_status_storage, &s_zen_status_buf );

    s_zen_ctx.cfg = AxisCfg {
        .drv = {
            .pul_n           = Pins::STEP2_PUL_N,
            .dir_n           = Pins::STEP2_DIR_N,
            .pulses_per_rev  = StepCfg::PULSES_PER_REV,
            .gear_ratio      = StepCfg::ZEN_GEAR_RATIO,
            .max_speed_dps   = StepCfg::MAX_SPEED_DPS,
            .default_speed_dps = StepCfg::DEFAULT_SPEED_DPS,
        },
        .cmd_q    = &g_stepper_zen_cmd_q,
        .status_q = &g_stepper_zen_status_q,
        .min_deg  = StepCfg::ZEN_MIN_DEG,
        .max_deg  = StepCfg::ZEN_MAX_DEG,
        .tag      = "zen",
    };

    task_create( axis_task_body, "step_zen", 512, &s_zen_ctx,
                 tskIDLE_PRIORITY + 3, s_zen_stack, &s_zen_tcb );
}

// -----------------------------------------------------------------------------
// Antenna state publisher
// -----------------------------------------------------------------------------

static StaticTask_t s_state_tcb;
static StackType_t  s_state_stack[ 512 ];

static void stepper_state_task( void* )
{
    TickType_t last_tick = xTaskGetTickCount();

    for ( ;; ) {
        StepperStatus az_st  = {};
        StepperStatus zen_st = {};
        StepperCmd    az_cmd = {};
        StepperCmd    zen_cmd = {};

        const bool have_az  = g_stepper_az_status_q
            && xQueuePeek( g_stepper_az_status_q, &az_st, 0 ) == pdTRUE;
        const bool have_zen = g_stepper_zen_status_q
            && xQueuePeek( g_stepper_zen_status_q, &zen_st, 0 ) == pdTRUE;

        if ( have_az && have_zen && mqtt_is_connected() ) {
            if ( g_stepper_az_cmd_q )
                xQueuePeek( g_stepper_az_cmd_q, &az_cmd, 0 );
            if ( g_stepper_zen_cmd_q )
                xQueuePeek( g_stepper_zen_cmd_q, &zen_cmd, 0 );

            MqttMessage m = {};
            snprintf( m.topic, sizeof(m.topic), "antenna/state" );
            snprintf( m.payload, sizeof(m.payload),
                      "{"
                      "\"timestamp\":%llu,"
                      "\"actual_az\":%.2f,\"actual_el\":%.2f,"
                      "\"target_az\":%.2f,\"target_el\":%.2f,"
                      "\"actual_az_mech\":%.2f,\"target_az_mech\":%.2f,"
                      "\"az_calibrated\":false,\"zen_calibrated\":false,"
                      "\"tracking_enabled\":false,"
                      "\"mode\":\"manual\","
                      "\"az_moving\":%s,\"zen_moving\":%s,"
                      "\"az_faulted\":%s,\"zen_faulted\":%s"
                      "}",
                      (unsigned long long)( time_us_64() / 1000u ),
                      (double)wrap_360( az_st.angle_deg ),
                      (double)zen_st.angle_deg,
                      (double)wrap_360( az_cmd.target_angle_deg ),
                      (double)zen_cmd.target_angle_deg,
                      (double)az_st.angle_deg,
                      (double)az_cmd.target_angle_deg,
                      az_st.moving ? "true" : "false",
                      zen_st.moving ? "true" : "false",
                      az_st.faulted ? "true" : "false",
                      zen_st.faulted ? "true" : "false" );
            xQueueSend( g_mqtt_queue, &m, 0 );
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS( StepCfg::STATE_PUBLISH_MS ) );
    }
}

void stepper_state_task_init()
{
    task_create( stepper_state_task, "step_state", 512, nullptr,
                 tskIDLE_PRIORITY + 2, s_state_stack, &s_state_tcb );
}

// -----------------------------------------------------------------------------
// Controller task
// -----------------------------------------------------------------------------

static StaticTask_t s_ctrl_tcb;
static StackType_t  s_ctrl_stack[ 512 ];

void stepper_ctrl_task_init()
{
    task_create( stepper_ctrl_task, "step_ctrl", 512, nullptr,
                 tskIDLE_PRIORITY + 2, s_ctrl_stack, &s_ctrl_tcb );
}
