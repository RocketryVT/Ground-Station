#include "stepper_task.hpp"
#include "tracker_state.hpp"
#include "shared.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Proto/mqtt_proto.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <string.h>

namespace StateCfg {
    static constexpr uint32_t PUBLISH_MS = 1000;
}

static float wrap_360( float deg )
{
    while ( deg < 0.0f )    deg += 360.0f;
    while ( deg >= 360.0f ) deg -= 360.0f;
    return deg;
}

static bool read_ahrs_display( const TrackerConfig& cfg,
                               float* actual_az,
                               float* actual_el,
                               const TrackerControlStatus& control )
{
    if ( !g_imu_q || ( !control.ahrs_az_used && !control.ahrs_el_used ) ) {
        return false;
    }

    ImuMsg imu = {};
    if ( xQueuePeek( g_imu_q, &imu, 0 ) != pdTRUE || !imu.valid ) {
        return false;
    }

    const uint64_t now_us = time_us_64();
    if ( imu.timestamp_us == 0 ||
         now_us - imu.timestamp_us > (uint64_t)cfg.ahrs_max_age_ms * 1000ull ) {
        return false;
    }

    if ( control.ahrs_el_used ) {
        *actual_el = imu.bar_rel_pitch;
    }
    if ( control.ahrs_az_used ) {
        *actual_az = imu.have_yaw_frame ? imu.yaw_frame_yaw360 : wrap_360( imu.euler[2] );
    }
    return true;
}

static StaticTask_t s_state_tcb;
static StackType_t s_state_stack[ 640 ];

static void stepper_state_task( void* )
{
    TickType_t last_tick = xTaskGetTickCount();

    for ( ;; ) {
        StepperStatus az_st = {};
        StepperStatus el_st = {};
        StepperCmd az_cmd = {};
        StepperCmd el_cmd = {};

        const bool have_az = g_stepper_az_status_q &&
            xQueuePeek( g_stepper_az_status_q, &az_st, 0 ) == pdTRUE;
        const bool have_el = g_stepper_zen_status_q &&
            xQueuePeek( g_stepper_zen_status_q, &el_st, 0 ) == pdTRUE;

        if ( have_az && have_el && mqtt_is_connected() ) {
            if ( g_stepper_az_cmd_q ) {
                xQueuePeek( g_stepper_az_cmd_q, &az_cmd, 0 );
            }
            if ( g_stepper_zen_cmd_q ) {
                xQueuePeek( g_stepper_zen_cmd_q, &el_cmd, 0 );
            }

            const TrackerConfig cfg = tracker_config_snapshot();
            const TrackerControlStatus control = tracker_control_status_snapshot();
            const TrackerCalibrationStatus cal = tracker_calibration_status_snapshot();
            const TrackerMode mode = tracker_mode();
            const bool armed = tracker_is_armed();

            float actual_az = wrap_360( az_st.angle_deg );
            float actual_el = el_st.angle_deg;
            read_ahrs_display( cfg, &actual_az, &actual_el, control );

            MqttMessage m = {};
            groundstation_AntennaState pb = groundstation_AntennaState_init_zero;
            pb.has_timestamp = true;        pb.timestamp = time_us_64() / 1000u;
            pb.has_actual_az = true;        pb.actual_az = actual_az;
            pb.has_actual_el = true;        pb.actual_el = actual_el;
            pb.has_target_az = true;        pb.target_az = wrap_360( az_cmd.target_angle_deg );
            pb.has_target_el = true;        pb.target_el = el_cmd.target_angle_deg;
            pb.has_actual_az_mech = true;   pb.actual_az_mech = az_st.angle_deg;
            pb.has_target_az_mech = true;   pb.target_az_mech = az_cmd.target_angle_deg;
            pb.has_az_calibrated = true;    pb.az_calibrated = cal.az_calibrated;
            pb.has_zen_calibrated = true;   pb.zen_calibrated = cal.el_calibrated;
            pb.has_tracking_enabled = true;
            pb.tracking_enabled = armed &&
                ( mode == TrackerMode::Auto || mode == TrackerMode::Scan );
            pb.has_az_moving = true;        pb.az_moving = az_st.moving;
            pb.has_zen_moving = true;       pb.zen_moving = el_st.moving;
            pb.has_az_faulted = true;       pb.az_faulted = az_st.faulted;
            pb.has_zen_faulted = true;      pb.zen_faulted = el_st.faulted;
            pb.has_mode = true;
            strncpy( pb.mode, tracker_mode_name( mode ), sizeof(pb.mode) - 1 );
            pb.has_armed = true;            pb.armed = armed;
            pb.has_gs_fresh = true;         pb.gs_fresh = control.gs_fresh;
            pb.has_target_fresh = true;     pb.target_fresh = control.target_fresh;
            pb.has_ahrs_el_used = true;     pb.ahrs_el_used = control.ahrs_el_used;
            pb.has_ahrs_az_used = true;     pb.ahrs_az_used = control.ahrs_az_used;
            pb.has_distance_m = true;       pb.distance_m = control.distance_m;
            pb.has_pointing_error_az = true;
            pb.pointing_error_az = control.pointing_error_az;
            pb.has_pointing_error_el = true;
            pb.pointing_error_el = control.pointing_error_el;
            pb.has_az_reference_deg = true;
            pb.az_reference_deg = cal.az_reference_deg;
            pb.has_el_reference_deg = true;
            pb.el_reference_deg = cal.el_reference_deg;
            pb.has_calibration_seq = true;
            pb.calibration_seq = cal.seq;
            pb.has_calibration_status = true;
            strncpy( pb.calibration_status, cal.status,
                     sizeof(pb.calibration_status) - 1 );

            if ( mqtt_encode_proto( m, "antenna/state",
                                    groundstation_AntennaState_fields, &pb ) ) {
                xQueueSend( g_mqtt_queue, &m, 0 );
            }
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS( StateCfg::PUBLISH_MS ) );
    }
}

void stepper_state_task_init()
{
    task_create( stepper_state_task, "step_state", 640, nullptr,
                 tskIDLE_PRIORITY + 2, s_state_stack, &s_state_tcb );
}
