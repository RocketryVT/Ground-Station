#include "stepper_task.hpp"
#include "cl57te.hpp"
#include "tracker_state.hpp"
#include "shared.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>

namespace StepHw {
    static constexpr uint32_t PULSES_PER_REV = 800;
    static constexpr float AZ_GEAR_RATIO = 50.0f;
    static constexpr float EL_GEAR_RATIO = 30.0f;
    static constexpr float EL_MOTOR_AT_HORIZON_DEG = 90.0f;
    // +1: increasing command elevation (toward zenith) drives the antenna up.
    // Verified 2026-06-05: with -1 the closed loop drove command to the +el rail
    // while the antenna physically went DOWN (command/physical anti-correlated),
    // confirming the direction was inverted.
    static constexpr float EL_MOTOR_SCALE = +1.0f;
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

QueueHandle_t g_stepper_az_cmd_q = nullptr;
QueueHandle_t g_stepper_zen_cmd_q = nullptr;
QueueHandle_t g_stepper_az_cal_q = nullptr;
QueueHandle_t g_stepper_zen_cal_q = nullptr;
QueueHandle_t g_stepper_az_status_q = nullptr;
QueueHandle_t g_stepper_zen_status_q = nullptr;

struct AxisCfg {
    Cl57te::Config drv;
    QueueHandle_t* cmd_q;
    QueueHandle_t* cal_q;
    QueueHandle_t* status_q;
    bool is_az;
    bool wrapped_limits;
    bool shortest_path;
    float motor_zero_cmd_deg;
    float motor_deg_per_cmd_deg;
    const char* tag;
};

struct AxisContext {
    Cl57te drv;
    AxisCfg cfg;
};

static float cmd_to_motor_angle( const AxisCfg& ax, float cmd_angle_deg )
{
    return ax.motor_zero_cmd_deg + ax.motor_deg_per_cmd_deg * cmd_angle_deg;
}

static float motor_to_cmd_angle( const AxisCfg& ax, float motor_angle_deg )
{
    return ( motor_angle_deg - ax.motor_zero_cmd_deg ) / ax.motor_deg_per_cmd_deg;
}

static float clamp_target( const AxisCfg& ax, float target_angle_deg )
{
    const TrackerConfig cfg = tracker_config_snapshot();
    const float min_deg = ax.is_az ? cfg.az_min_deg : cfg.el_min_deg;
    const float max_deg = ax.is_az ? cfg.az_max_deg : cfg.el_max_deg;

    if ( ax.wrapped_limits ) {
        return clamp_wrapped_angle( target_angle_deg, min_deg, max_deg );
    }

    if ( target_angle_deg < min_deg ) return min_deg;
    if ( target_angle_deg > max_deg ) return max_deg;
    return target_angle_deg;
}

static float clamp_speed( float speed_dps )
{
    const TrackerConfig cfg = tracker_config_snapshot();
    float speed = ( speed_dps > 0.0f ) ? speed_dps : cfg.default_speed_dps;
    if ( speed > cfg.max_speed_dps ) speed = cfg.max_speed_dps;
    if ( speed < 1.0f ) speed = 1.0f;
    return speed;
}

static void axis_task_body( void* arg )
{
    auto* ctx = static_cast<AxisContext*>( arg );
    Cl57te& drv = ctx->drv;
    const AxisCfg& ax = ctx->cfg;

    drv.init( ax.drv );
    vTaskDelay( pdMS_TO_TICKS(200) );

    log_print( "[%s] ready steps/deg=%.2f\n", ax.tag, drv.steps_per_deg() );

    float target_angle_deg = 0.0f;
    bool have_target = false;
    bool target_dirty = false;
    bool move_pending_completion = false;

    for ( ;; ) {
        StepperCalibrationCmd cal = {};
        if ( xQueueReceive( *ax.cal_q, &cal, 0 ) == pdTRUE ) {
            if ( cal.clear ) {
                drv.stop();
                target_angle_deg = 0.0f;
                have_target = false;
                target_dirty = false;
                move_pending_completion = false;
            } else if ( cal.set_current_angle ) {
                const float clamped_ref = clamp_target( ax, cal.current_angle_deg );
                const float motor_ref = cmd_to_motor_angle( ax, clamped_ref );
                drv.set_current_angle( motor_ref );
                target_angle_deg = clamped_ref;
                have_target = true;
                target_dirty = false;
                move_pending_completion = false;
                if ( ax.is_az ) {
                    tracker_mark_az_calibrated( clamped_ref, "az reference set" );
                } else {
                    tracker_mark_el_calibrated( clamped_ref, "el reference set" );
                }
                log_print( "[%s] calibrated reference=%.2f motor=%.2f\n",
                           ax.tag, (double)clamped_ref, (double)motor_ref );
            }
        }

        StepperCmd cmd = {};
        const bool have_cmd = xQueuePeek( *ax.cmd_q, &cmd, 0 ) == pdTRUE;
        if ( have_cmd ) {
            if ( cmd.stop ) {
                drv.stop();
                target_dirty = false;
                move_pending_completion = false;
            } else {
                const float clamped = clamp_target( ax, cmd.target_angle_deg );
                if ( !have_target || fabsf( clamped - target_angle_deg ) > 0.001f ) {
                    target_angle_deg = clamped;
                    have_target = true;
                    target_dirty = true;
                }
            }
        }

        if ( !drv.is_moving() ) {
            if ( move_pending_completion ) {
                move_pending_completion = false;
                log_print( "[%s] move complete pos_steps=%ld angle=%.2f motor_angle=%.2f\n",
                           ax.tag,
                           (long)drv.pos_steps(),
                           (double)motor_to_cmd_angle( ax, drv.angle_deg() ),
                           (double)drv.angle_deg() );
            }
        }

        if ( ( !have_cmd || !cmd.stop ) && target_dirty && have_target ) {
            StepperCmd latest = {};
            xQueuePeek( *ax.cmd_q, &latest, 0 );

            const float before_angle = motor_to_cmd_angle( ax, drv.angle_deg() );
            const float target_motor_angle_deg =
                cmd_to_motor_angle( ax, target_angle_deg );
            const float motor_delta_deg = ax.shortest_path
                ? normalize_delta_180( wrap_360( target_motor_angle_deg )
                                       - wrap_360( drv.angle_deg() ) )
                : target_motor_angle_deg - drv.angle_deg();
            const int32_t est_delta_steps =
                (int32_t)roundf( motor_delta_deg * drv.steps_per_deg() );

            if ( drv.set_angle( target_motor_angle_deg,
                                clamp_speed( latest.speed_dps ),
                                ax.shortest_path ) ) {
                target_dirty = false;
                if ( drv.is_moving() ) {
                    move_pending_completion = true;
                    log_print( "[%s] target=%.2f from=%.2f motor_target=%.2f delta_steps=%ld speed_dps=%.2f dir_gpio=%d\n",
                               ax.tag,
                               (double)target_angle_deg,
                               (double)before_angle,
                               (double)target_motor_angle_deg,
                               (long)est_delta_steps,
                               (double)clamp_speed( latest.speed_dps ),
                               drv.dir_gpio_level() ? 1 : 0 );
                }
            }
        }

        StepperStatus st = {};
        st.angle_deg = motor_to_cmd_angle( ax, drv.angle_deg() );
        st.moving = drv.is_moving();
        st.faulted = false;
        st.enabled = true;
        st.timestamp_us = time_us_64();
        xQueueOverwrite( *ax.status_q, &st );

        vTaskDelay( pdMS_TO_TICKS(10) );
    }
}

static StaticQueue_t s_az_cmd_buf, s_zen_cmd_buf;
static StaticQueue_t s_az_cal_buf, s_zen_cal_buf;
static StaticQueue_t s_az_status_buf, s_zen_status_buf;

static uint8_t s_az_cmd_storage[ sizeof(StepperCmd) ];
static uint8_t s_zen_cmd_storage[ sizeof(StepperCmd) ];
static uint8_t s_az_cal_storage[ sizeof(StepperCalibrationCmd) ];
static uint8_t s_zen_cal_storage[ sizeof(StepperCalibrationCmd) ];
static uint8_t s_az_status_storage[ sizeof(StepperStatus) ];
static uint8_t s_zen_status_storage[ sizeof(StepperStatus) ];

static AxisContext s_az_ctx;
static StaticTask_t s_az_tcb;
static StackType_t s_az_stack[ 512 ];

void stepper_az_task_init()
{
    g_stepper_az_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_az_cmd_storage, &s_az_cmd_buf );
    g_stepper_az_cal_q = xQueueCreateStatic(
        1, sizeof(StepperCalibrationCmd), s_az_cal_storage, &s_az_cal_buf );
    g_stepper_az_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_az_status_storage, &s_az_status_buf );

    const TrackerConfig cfg = tracker_config_snapshot();
    s_az_ctx.cfg = AxisCfg {
        .drv = {
            .pul_n = Pins::STEP1_PUL_N,
            .dir_n = Pins::STEP1_DIR_N,
            .pulses_per_rev = StepHw::PULSES_PER_REV,
            .gear_ratio = StepHw::AZ_GEAR_RATIO,
            .max_speed_dps = cfg.max_speed_dps,
            .default_speed_dps = cfg.default_speed_dps,
        },
        .cmd_q = &g_stepper_az_cmd_q,
        .cal_q = &g_stepper_az_cal_q,
        .status_q = &g_stepper_az_status_q,
        .is_az = true,
        .wrapped_limits = false,
        .shortest_path = true,
        .motor_zero_cmd_deg = 0.0f,
        .motor_deg_per_cmd_deg = 1.0f,
        .tag = "az",
    };

    task_create( axis_task_body, "step_az", 512, &s_az_ctx,
                 tskIDLE_PRIORITY + 3, s_az_stack, &s_az_tcb );
}

static AxisContext s_el_ctx;
static StaticTask_t s_el_tcb;
static StackType_t s_el_stack[ 512 ];

void stepper_zen_task_init()
{
    g_stepper_zen_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_zen_cmd_storage, &s_zen_cmd_buf );
    g_stepper_zen_cal_q = xQueueCreateStatic(
        1, sizeof(StepperCalibrationCmd), s_zen_cal_storage, &s_zen_cal_buf );
    g_stepper_zen_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_zen_status_storage, &s_zen_status_buf );

    const TrackerConfig cfg = tracker_config_snapshot();
    s_el_ctx.cfg = AxisCfg {
        .drv = {
            .pul_n = Pins::STEP2_PUL_N,
            .dir_n = Pins::STEP2_DIR_N,
            .pulses_per_rev = StepHw::PULSES_PER_REV,
            .gear_ratio = StepHw::EL_GEAR_RATIO,
            .max_speed_dps = cfg.max_speed_dps,
            .default_speed_dps = cfg.default_speed_dps,
        },
        .cmd_q = &g_stepper_zen_cmd_q,
        .cal_q = &g_stepper_zen_cal_q,
        .status_q = &g_stepper_zen_status_q,
        .is_az = false,
        .wrapped_limits = false,
        .shortest_path = false,
        .motor_zero_cmd_deg = StepHw::EL_MOTOR_AT_HORIZON_DEG,
        .motor_deg_per_cmd_deg = StepHw::EL_MOTOR_SCALE,
        .tag = "el",
    };

    task_create( axis_task_body, "step_el", 512, &s_el_ctx,
                 tskIDLE_PRIORITY + 3, s_el_stack, &s_el_tcb );
}
