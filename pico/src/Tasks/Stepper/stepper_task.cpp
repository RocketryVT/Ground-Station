#include "stepper_task.hpp"
#include "cl57te.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Proto/mqtt_proto.hpp"

#include "math_utils/GroundStationMath.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Motor / drive configuration
//
// CL57TE DIP-switch: 800 pulses/rev.
// Total gear reduction = 10:1 planetary gearbox × belt ratio per axis.
// -----------------------------------------------------------------------------

namespace StepCfg {
    static constexpr uint32_t PULSES_PER_REV       = 800;

    // Per-axis gear ratios  (10:1 planetary gearbox × belt ratio)
    static constexpr float    AZ_GEAR_RATIO         = 50.0f;   // 10:1 × 3:1 belt  (azimuth)
    static constexpr float    EL_GEAR_RATIO         = 30.0f;   // 10:1 × 5:1 belt  (elevation)

    static constexpr float    MAX_SPEED_DPS         = 90.0f;
    static constexpr float    DEFAULT_SPEED_DPS     = 30.0f;

    // Azimuth: full 360° travel (no cable-stop limits while yaw sensor is offline).
    static constexpr float    AZ_MIN_DEG            =   0.0f;
    static constexpr float    AZ_MAX_DEG            = 360.0f;

    // Elevation command convention: 0° = horizon, 90° = straight up.
    // The motor's mechanical frame is kept internal: motor_deg = 90 - elevation_deg.
    static constexpr float    EL_MIN_DEG            = -10.0f;
    static constexpr float    EL_MAX_DEG            =  90.0f;
    static constexpr float    EL_MOTOR_AT_HORIZON_DEG = 90.0f;
    static constexpr float    EL_MOTOR_SCALE        = -1.0f;

    static constexpr uint32_t STATE_PUBLISH_MS      = 1000;
}

static float wrap_360( float deg )
{
    while ( deg < 0.0f )    deg += 360.0f;
    while ( deg >= 360.0f ) deg -= 360.0f;
    return deg;
}

static float normalize_delta_180( float deg )
{
    while ( deg > 180.0f )   deg -= 360.0f;
    while ( deg <= -180.0f ) deg += 360.0f;
    return deg;
}

static bool angle_in_wrapped_range( float deg, float min_deg, float max_deg )
{
    const float angle = wrap_360( deg );
    const float min_w = wrap_360( min_deg );
    const float max_w = wrap_360( max_deg );

    if ( min_w <= max_w ) {
        return angle >= min_w && angle <= max_w;
    }
    return angle >= min_w || angle <= max_w;
}

static float clamp_wrapped_angle( float deg, float min_deg, float max_deg )
{
    const float angle = wrap_360( deg );
    const float min_w = wrap_360( min_deg );
    const float max_w = wrap_360( max_deg );

    if ( angle_in_wrapped_range( angle, min_w, max_w ) ) {
        return angle;
    }

    const float to_min = fabsf( normalize_delta_180( min_w - angle ) );
    const float to_max = fabsf( normalize_delta_180( max_w - angle ) );
    return to_min <= to_max ? min_w : max_w;
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
    bool  wrapped_limits;       // true when allowed range crosses 0°
    bool  shortest_path;        // true for modulo-360 axes such as azimuth
    float motor_zero_cmd_deg;    // motor angle when public command angle is 0°
    float motor_deg_per_cmd_deg; // +1 for same frame, -1 for inverted elevation
    const char*    tag;
};

struct AxisContext {
    Cl57te   drv;
    AxisCfg  cfg;
};

static float cmd_to_motor_angle( const AxisCfg& ax, float cmd_angle_deg )
{
    return ax.motor_zero_cmd_deg + ax.motor_deg_per_cmd_deg * cmd_angle_deg;
}

static float motor_to_cmd_angle( const AxisCfg& ax, float motor_angle_deg )
{
    return ( motor_angle_deg - ax.motor_zero_cmd_deg ) / ax.motor_deg_per_cmd_deg;
}

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

    float    target_angle_deg = 0.0f;
    bool     have_target = false;
    bool     target_dirty = false;
    bool     move_pending_completion = false;

    for ( ;; ) {
        // -- Process latest command -------------------------------------------
        StepperCmd cmd = {};
        if ( xQueuePeek( *ax.cmd_q, &cmd, 0 ) == pdTRUE ) {

            if ( cmd.stop ) {
                drv.stop();
                target_dirty = false;
                move_pending_completion = false;

            } else {
                float clamped = ax.wrapped_limits
                    ? clamp_wrapped_angle( cmd.target_angle_deg, ax.min_deg, ax.max_deg )
                    : cmd.target_angle_deg;
                if ( !ax.wrapped_limits ) {
                    if ( clamped < ax.min_deg ) clamped = ax.min_deg;
                    if ( clamped > ax.max_deg ) clamped = ax.max_deg;
                }

                if ( !have_target
                     || fabsf( clamped - target_angle_deg ) > 0.001f ) {
                    target_angle_deg = clamped;
                    have_target = true;
                    target_dirty = true;
                    if ( drv.is_moving() ) {
                        drv.stop();
                        move_pending_completion = false;
                        log_print( "[%s] move interrupted for new target=%.2f\n",
                                   ax.tag,
                                   (double)target_angle_deg );
                    }
                }
            }
        }

        // -- Issue move if needed ---------------------------------------------
        if ( !cmd.stop && !drv.is_moving() ) {
            if ( move_pending_completion ) {
                int32_t pos_steps = drv.pos_steps();
                move_pending_completion = false;
                log_print( "[%s] move complete pos_steps=%ld angle=%.2f motor_angle=%.2f\n",
                           ax.tag,
                           (long)pos_steps,
                           (double)motor_to_cmd_angle( ax, drv.angle_deg() ),
                           (double)drv.angle_deg() );
            }

            if ( target_dirty && have_target ) {
                StepperCmd c = {};
                xQueuePeek( *ax.cmd_q, &c, 0 );

                const float before_angle = motor_to_cmd_angle( ax, drv.angle_deg() );
                const float target_motor_angle_deg =
                    cmd_to_motor_angle( ax, target_angle_deg );
                const float motor_delta_deg = ax.shortest_path
                    ? normalize_delta_180( wrap_360( target_motor_angle_deg )
                                           - wrap_360( drv.angle_deg() ) )
                    : target_motor_angle_deg - drv.angle_deg();
                const int32_t est_delta_steps =
                    (int32_t)roundf( motor_delta_deg * drv.steps_per_deg() );
                if ( drv.set_angle( target_motor_angle_deg, c.speed_dps, ax.shortest_path ) ) {
                    target_dirty = false;
                    if ( drv.is_moving() ) {
                        move_pending_completion = true;
                        log_print( "[%s] move target=%.2f from=%.2f motor_target=%.2f delta_steps=%ld speed_dps=%.2f dir_gpio=%u dir_readback=%u\n",
                                   ax.tag,
                                   (double)target_angle_deg,
                                   (double)before_angle,
                                   (double)target_motor_angle_deg,
                                   (long)est_delta_steps,
                                   (double)c.speed_dps,
                                   (unsigned)drv.dir_pin(),
                                   drv.dir_gpio_level() ? 1u : 0u );
                    }
                }
            }
        }

        // -- Publish status ---------------------------------------------------
        StepperStatus st = {};
        st.angle_deg    = motor_to_cmd_angle( ax, drv.angle_deg() );
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
// Reads ground-station and rocket location queues, computes az/elevation, posts cmds.
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
            double el = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            StepperCmd az_cmd  { wrap_360( static_cast<float>( az ) ), 0.0f, false };
            StepperCmd zen_cmd { static_cast<float>( el ), 0.0f, false };

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
// 10:1 planetary gearbox × 3:1 belt = 30:1 total, full 360° travel (limits disabled)
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
        .wrapped_limits = false,
        .shortest_path = true,
        .motor_zero_cmd_deg = 0.0f,
        .motor_deg_per_cmd_deg = 1.0f,
        .tag      = "az",
    };

    task_create( axis_task_body, "step_az", 512, &s_az_ctx,
                 tskIDLE_PRIORITY + 3, s_az_stack, &s_az_tcb );
}

// -----------------------------------------------------------------------------
// Elevation axis  (STEP2 – PUL- GPIO 6, DIR- GPIO 7)
// Public command: 0° = horizon, 90° = straight up.
// Internal motor frame: 0° = straight up, 90° = horizon, 180° = straight down.
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
            .gear_ratio      = StepCfg::EL_GEAR_RATIO,
            .max_speed_dps   = StepCfg::MAX_SPEED_DPS,
            .default_speed_dps = StepCfg::DEFAULT_SPEED_DPS,
        },
        .cmd_q    = &g_stepper_zen_cmd_q,
        .status_q = &g_stepper_zen_status_q,
        .min_deg  = StepCfg::EL_MIN_DEG,
        .max_deg  = StepCfg::EL_MAX_DEG,
        .wrapped_limits = false,
        .shortest_path = false,
        .motor_zero_cmd_deg = StepCfg::EL_MOTOR_AT_HORIZON_DEG,
        .motor_deg_per_cmd_deg = StepCfg::EL_MOTOR_SCALE,
        .tag      = "el",
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
            groundstation_AntennaState pb = groundstation_AntennaState_init_zero;
            pb.has_timestamp = true;        pb.timestamp = time_us_64() / 1000u;
            pb.has_actual_az = true;        pb.actual_az = wrap_360( az_st.angle_deg );
            pb.has_actual_el = true;        pb.actual_el = zen_st.angle_deg;
            pb.has_target_az = true;        pb.target_az = wrap_360( az_cmd.target_angle_deg );
            pb.has_target_el = true;        pb.target_el = zen_cmd.target_angle_deg;
            pb.has_actual_az_mech = true;   pb.actual_az_mech = az_st.angle_deg;
            pb.has_target_az_mech = true;   pb.target_az_mech = az_cmd.target_angle_deg;
            pb.has_az_calibrated = true;    pb.az_calibrated = false;
            pb.has_zen_calibrated = true;   pb.zen_calibrated = false;
            pb.has_tracking_enabled = true; pb.tracking_enabled = false;
            pb.has_az_moving = true;        pb.az_moving = az_st.moving;
            pb.has_zen_moving = true;       pb.zen_moving = zen_st.moving;
            pb.has_az_faulted = true;       pb.az_faulted = az_st.faulted;
            pb.has_zen_faulted = true;      pb.zen_faulted = zen_st.faulted;
            pb.has_mode = true;
            strncpy( pb.mode, "manual", sizeof(pb.mode) - 1 );

            if ( mqtt_encode_proto( m, "antenna/state",
                                    groundstation_AntennaState_fields, &pb ) )
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
