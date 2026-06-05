#include "stepper_task.hpp"
#include "tracker_state.hpp"
#include "shared.hpp"

#include "math_utils/GroundStationMath.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>

static constexpr TickType_t kControlPeriodTicks = pdMS_TO_TICKS(20);

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

static float clamp_float( float v, float min_v, float max_v )
{
    if ( v < min_v ) return min_v;
    if ( v > max_v ) return max_v;
    return v;
}

static bool is_fresh( const LocationMsg& loc, uint64_t now_us, uint32_t timeout_ms )
{
    if ( loc.timestamp_us == 0 ) return false;
    const uint64_t age_us = now_us - loc.timestamp_us;
    return age_us <= (uint64_t)timeout_ms * 1000ull;
}

static void publish_stop()
{
    StepperCmd stop = {};
    stop.stop = true;
    if ( g_stepper_az_cmd_q ) xQueueOverwrite( g_stepper_az_cmd_q, &stop );
    if ( g_stepper_zen_cmd_q ) xQueueOverwrite( g_stepper_zen_cmd_q, &stop );
}

static void publish_pointing_command( float az_deg, float el_deg, float speed_dps )
{
    StepperCmd az_cmd { wrap_360( az_deg ), speed_dps, false };
    StepperCmd el_cmd { el_deg, speed_dps, false };
    if ( g_stepper_az_cmd_q ) xQueueOverwrite( g_stepper_az_cmd_q, &az_cmd );
    if ( g_stepper_zen_cmd_q ) xQueueOverwrite( g_stepper_zen_cmd_q, &el_cmd );
}

static bool read_ahrs_actual( const TrackerConfig& cfg,
                              float* actual_az,
                              float* actual_el,
                              bool* az_used,
                              bool* el_used )
{
    *az_used = false;
    *el_used = false;

    if ( !g_imu_q ) return false;

    ImuMsg imu = {};
    if ( xQueuePeek( g_imu_q, &imu, 0 ) != pdTRUE || !imu.valid ) return false;

    const uint64_t now_us = time_us_64();
    if ( imu.timestamp_us == 0 ||
         now_us - imu.timestamp_us > (uint64_t)cfg.ahrs_max_age_ms * 1000ull ) {
        return false;
    }

    if ( cfg.use_ahrs_el ) {
        *actual_el = imu.euler[1];
        *el_used = true;
    }

    if ( cfg.use_ahrs_az ) {
        if ( imu.have_yaw_frame ) {
            *actual_az = imu.yaw_frame_yaw360;
            *az_used = true;
        } else {
            *actual_az = wrap_360( imu.euler[2] );
            *az_used = true;
        }
    }

    return *az_used || *el_used;
}

static void apply_ahrs_feedback( const TrackerConfig& cfg,
                                 float desired_az,
                                 float desired_el,
                                 float* cmd_az,
                                 float* cmd_el,
                                 TrackerControlStatus* status )
{
    StepperStatus az_st = {};
    StepperStatus el_st = {};
    const bool have_az_st = g_stepper_az_status_q &&
        xQueuePeek( g_stepper_az_status_q, &az_st, 0 ) == pdTRUE;
    const bool have_el_st = g_stepper_zen_status_q &&
        xQueuePeek( g_stepper_zen_status_q, &el_st, 0 ) == pdTRUE;

    float actual_az = have_az_st ? wrap_360( az_st.angle_deg ) : desired_az;
    float actual_el = have_el_st ? el_st.angle_deg : desired_el;
    bool az_used = false;
    bool el_used = false;
    read_ahrs_actual( cfg, &actual_az, &actual_el, &az_used, &el_used );

    const float az_error = normalize_delta_180( desired_az - actual_az );
    const float el_error = desired_el - actual_el;
    status->pointing_error_az = az_error;
    status->pointing_error_el = el_error;
    status->ahrs_az_used = az_used;
    status->ahrs_el_used = el_used;

    if ( az_used && have_az_st ) {
        const float correction = clamp_float( az_error * cfg.ahrs_feedback_gain,
                                             -cfg.ahrs_max_correction_deg,
                                             cfg.ahrs_max_correction_deg );
        *cmd_az = wrap_360( az_st.angle_deg + correction );
    }

    if ( el_used && have_el_st ) {
        const float correction = clamp_float( el_error * cfg.ahrs_feedback_gain,
                                             -cfg.ahrs_max_correction_deg,
                                             cfg.ahrs_max_correction_deg );
        *cmd_el = el_st.angle_deg + correction;
    }
}

static void update_scan( float dt_s, const TrackerConfig& cfg, float* scan_az,
                         float* scan_el, bool* scan_el_reverse )
{
    *scan_az = wrap_360( *scan_az + cfg.scan_speed_az_dps * dt_s );

    const float dir = *scan_el_reverse ? -1.0f : 1.0f;
    *scan_el += cfg.scan_speed_el_dps * dt_s * dir;
    if ( *scan_el > cfg.el_max_deg ) {
        *scan_el = cfg.el_max_deg;
        *scan_el_reverse = true;
    } else if ( *scan_el < cfg.el_min_deg ) {
        *scan_el = cfg.el_min_deg;
        *scan_el_reverse = false;
    }
}

static StaticTask_t s_ctrl_tcb;
static StackType_t s_ctrl_stack[ 768 ];

static void stepper_ctrl_task( void* )
{
    log_print( "[tracker] controller started; default mode=%s armed=%u\n",
               tracker_mode_name( tracker_mode() ),
               tracker_is_armed() ? 1u : 0u );

    float scan_az = 0.0f;
    float scan_el = 0.0f;
    bool scan_el_reverse = false;
    uint64_t last_us = time_us_64();

    for ( ;; ) {
        const uint64_t now_us = time_us_64();
        const float dt_s = (float)( now_us - last_us ) * 1.0e-6f;
        last_us = now_us;

        const TrackerConfig cfg = tracker_config_snapshot();
        const TrackerMode mode = tracker_mode();
        const bool armed = tracker_is_armed();

        LocationMsg gs = {};
        LocationMsg rkt = {};
        const bool have_gs = g_gs_location_q &&
            xQueuePeek( g_gs_location_q, &gs, 0 ) == pdTRUE;
        const bool have_rkt = g_rocket_location_q &&
            xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE;
        const bool gs_fresh = have_gs && is_fresh( gs, now_us, cfg.gs_timeout_ms );
        const bool target_fresh = have_rkt && is_fresh( rkt, now_us, cfg.target_timeout_ms );

        TrackerControlStatus status = {};
        status.gs_fresh = gs_fresh;
        status.target_fresh = target_fresh;

        if ( !armed || mode == TrackerMode::Stop || mode == TrackerMode::Fault ) {
            publish_stop();
            tracker_set_control_status( status );
            vTaskDelay( kControlPeriodTicks );
            continue;
        }

        if ( mode == TrackerMode::Manual || mode == TrackerMode::ServoTest ) {
            tracker_set_control_status( status );
            vTaskDelay( kControlPeriodTicks );
            continue;
        }

        if ( !tracker_axes_calibrated() ) {
            publish_stop();
            tracker_set_control_status( status );
            vTaskDelay( kControlPeriodTicks );
            continue;
        }

        bool should_scan = ( mode == TrackerMode::Scan );
        if ( mode == TrackerMode::Auto && ( !gs_fresh || !target_fresh ) ) {
            should_scan = cfg.scan_on_loss && gs_fresh;
        }

        if ( should_scan ) {
            update_scan( dt_s, cfg, &scan_az, &scan_el, &scan_el_reverse );
            publish_pointing_command( scan_az, scan_el, cfg.default_speed_dps );
            tracker_set_control_status( status );
            vTaskDelay( kControlPeriodTicks );
            continue;
        }

        if ( mode == TrackerMode::Auto && gs_fresh && target_fresh ) {
            rocket_math::Location station{ gs.lat, gs.lon, gs.alt_m };
            rocket_math::Location rocket{ rkt.lat, rkt.lon, rkt.alt_m };
            const double distance =
                rocket_math::GroundStationMath::haversineDistance( station, rocket );
            status.distance_m = (float)distance;

            if ( distance >= cfg.distance_min_m ) {
                float desired_az = wrap_360( (float)
                    rocket_math::GroundStationMath::calculateAzimuth( station, rocket )
                    + cfg.yaw_trim_deg );
                float desired_el = clamp_float( (float)
                    rocket_math::GroundStationMath::calculateElevation( station, rocket )
                    + cfg.el_trim_deg,
                    cfg.el_min_deg,
                    cfg.el_max_deg );
                float cmd_az = desired_az;
                float cmd_el = desired_el;

                apply_ahrs_feedback( cfg, desired_az, desired_el,
                                     &cmd_az, &cmd_el, &status );
                publish_pointing_command( cmd_az, cmd_el, cfg.default_speed_dps );
                scan_az = cmd_az;
                scan_el = cmd_el;
            }
        }

        tracker_set_control_status( status );
        vTaskDelay( kControlPeriodTicks );
    }
}

void stepper_ctrl_task_init()
{
    task_create( stepper_ctrl_task, "step_ctrl", 768, nullptr,
                 tskIDLE_PRIORITY + 2, s_ctrl_stack, &s_ctrl_tcb );
}
