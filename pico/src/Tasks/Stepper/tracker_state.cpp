#include "tracker_state.hpp"

#include "FreeRTOS.h"
#include "task.h"

#include <math.h>
#include <string.h>

static TrackerConfig s_config;
static TrackerMode s_mode = TrackerMode::Stop;
static bool s_armed = false;
static TrackerControlStatus s_control_status;
static TrackerCalibrationStatus s_cal_status;
static float s_manual_az_deg = 0.0f;
static float s_manual_el_deg = 0.0f;
static bool  s_manual_az_valid = false;
static bool  s_manual_el_valid = false;
static bool  s_manual_az_absolute_ahrs = false;
static bool  s_manual_el_absolute_ahrs = false;

static float sane_float( float value, float fallback )
{
    return ( isfinite( value ) ) ? value : fallback;
}

static float min_float( float a, float b )
{
    return a < b ? a : b;
}

static float max_float( float a, float b )
{
    return a > b ? a : b;
}

static TrackerConfig sanitize( TrackerConfig cfg )
{
    cfg.az_min_deg = sane_float( cfg.az_min_deg, 0.0f );
    cfg.az_max_deg = sane_float( cfg.az_max_deg, 360.0f );
    cfg.el_min_deg = sane_float( cfg.el_min_deg, -10.0f );
    cfg.el_max_deg = sane_float( cfg.el_max_deg, 90.0f );
    if ( cfg.el_min_deg > cfg.el_max_deg ) {
        const float tmp = cfg.el_min_deg;
        cfg.el_min_deg = cfg.el_max_deg;
        cfg.el_max_deg = tmp;
    }

    cfg.default_speed_dps = max_float( 1.0f, sane_float( cfg.default_speed_dps, 30.0f ) );
    cfg.max_speed_dps = max_float( cfg.default_speed_dps, sane_float( cfg.max_speed_dps, 90.0f ) );
    cfg.scan_speed_az_dps = max_float( 0.1f, sane_float( cfg.scan_speed_az_dps, 20.0f ) );
    cfg.scan_speed_el_dps = max_float( 0.1f, sane_float( cfg.scan_speed_el_dps, 5.0f ) );

    cfg.gs_timeout_ms = (uint32_t)max_float( 1000.0f, (float)cfg.gs_timeout_ms );
    cfg.target_timeout_ms = (uint32_t)max_float( 250.0f, (float)cfg.target_timeout_ms );
    cfg.distance_min_m = max_float( 0.0f, sane_float( cfg.distance_min_m, 3.0f ) );

    cfg.ahrs_max_age_ms = max_float( 20.0f, sane_float( cfg.ahrs_max_age_ms, 250.0f ) );
    cfg.ahrs_feedback_gain = min_float( 1.0f, max_float( 0.0f, sane_float( cfg.ahrs_feedback_gain, 0.35f ) ) );
    cfg.ahrs_max_correction_deg =
        max_float( 0.0f, sane_float( cfg.ahrs_max_correction_deg, 8.0f ) );
    cfg.base_altitude_m = sane_float( cfg.base_altitude_m, 880.0f );
    cfg.altitude_full_scale_m =
        max_float( 1.0f, sane_float( cfg.altitude_full_scale_m, 1000.0f ) );
    return cfg;
}

TrackerConfig tracker_config_snapshot()
{
    TrackerConfig cfg;
    taskENTER_CRITICAL();
    cfg = s_config;
    taskEXIT_CRITICAL();
    return cfg;
}

TrackerControlStatus tracker_control_status_snapshot()
{
    TrackerControlStatus status;
    taskENTER_CRITICAL();
    status = s_control_status;
    taskEXIT_CRITICAL();
    return status;
}

TrackerCalibrationStatus tracker_calibration_status_snapshot()
{
    TrackerCalibrationStatus status;
    taskENTER_CRITICAL();
    status = s_cal_status;
    taskEXIT_CRITICAL();
    return status;
}

void tracker_apply_config_command( const groundstation_TrackerConfigCommand& cmd )
{
    taskENTER_CRITICAL();
    TrackerConfig cfg = s_config;
    if ( cmd.has_yaw_trim_deg ) cfg.yaw_trim_deg = cmd.yaw_trim_deg;
    if ( cmd.has_el_trim_deg ) cfg.el_trim_deg = cmd.el_trim_deg;
    if ( cmd.has_az_min_deg ) cfg.az_min_deg = cmd.az_min_deg;
    if ( cmd.has_az_max_deg ) cfg.az_max_deg = cmd.az_max_deg;
    if ( cmd.has_el_min_deg ) cfg.el_min_deg = cmd.el_min_deg;
    if ( cmd.has_el_max_deg ) cfg.el_max_deg = cmd.el_max_deg;
    if ( cmd.has_default_speed_dps ) cfg.default_speed_dps = cmd.default_speed_dps;
    if ( cmd.has_max_speed_dps ) cfg.max_speed_dps = cmd.max_speed_dps;
    if ( cmd.has_scan_speed_az_dps ) cfg.scan_speed_az_dps = cmd.scan_speed_az_dps;
    if ( cmd.has_scan_speed_el_dps ) cfg.scan_speed_el_dps = cmd.scan_speed_el_dps;
    if ( cmd.has_gs_timeout_ms ) cfg.gs_timeout_ms = cmd.gs_timeout_ms;
    if ( cmd.has_target_timeout_ms ) cfg.target_timeout_ms = cmd.target_timeout_ms;
    if ( cmd.has_distance_min_m ) cfg.distance_min_m = cmd.distance_min_m;
    if ( cmd.has_scan_on_loss ) cfg.scan_on_loss = cmd.scan_on_loss;
    if ( cmd.has_use_ahrs_el ) cfg.use_ahrs_el = cmd.use_ahrs_el;
    if ( cmd.has_use_ahrs_az ) cfg.use_ahrs_az = cmd.use_ahrs_az;
    if ( cmd.has_ahrs_max_age_ms ) cfg.ahrs_max_age_ms = cmd.ahrs_max_age_ms;
    if ( cmd.has_ahrs_feedback_gain ) cfg.ahrs_feedback_gain = cmd.ahrs_feedback_gain;
    if ( cmd.has_ahrs_max_correction_deg ) {
        cfg.ahrs_max_correction_deg = cmd.ahrs_max_correction_deg;
    }
    s_config = sanitize( cfg );
    taskEXIT_CRITICAL();
}

void tracker_set_altitude_profile( float base_altitude_m, float full_scale_m )
{
    taskENTER_CRITICAL();
    TrackerConfig cfg = s_config;
    cfg.base_altitude_m = base_altitude_m;
    cfg.altitude_full_scale_m = full_scale_m;
    s_config = sanitize( cfg );
    taskEXIT_CRITICAL();
}

TrackerMode tracker_mode()
{
    TrackerMode mode;
    taskENTER_CRITICAL();
    mode = s_mode;
    taskEXIT_CRITICAL();
    return mode;
}

const char* tracker_mode_name( TrackerMode mode )
{
    switch ( mode ) {
        case TrackerMode::Stop:      return "stop";
        case TrackerMode::Manual:    return "manual";
        case TrackerMode::Auto:      return "auto";
        case TrackerMode::Scan:      return "scan";
        case TrackerMode::ServoTest: return "servotest";
        case TrackerMode::Fault:     return "fault";
    }
    return "unknown";
}

void tracker_set_mode( TrackerMode mode )
{
    taskENTER_CRITICAL();
    s_mode = mode;
    // Manual targets only make sense while actively in Manual mode.
    if ( mode != TrackerMode::Manual ) {
        s_manual_az_valid = false;
        s_manual_el_valid = false;
        s_manual_az_absolute_ahrs = false;
        s_manual_el_absolute_ahrs = false;
    }
    taskEXIT_CRITICAL();
}

void tracker_set_manual_target( bool is_az, float deg, bool absolute_ahrs )
{
    taskENTER_CRITICAL();
    if ( is_az ) {
        s_manual_az_deg = deg;
        s_manual_az_valid = true;
        s_manual_az_absolute_ahrs = absolute_ahrs;
    } else {
        s_manual_el_deg = deg;
        s_manual_el_valid = true;
        s_manual_el_absolute_ahrs = absolute_ahrs;
    }
    taskEXIT_CRITICAL();
}

bool tracker_get_manual_target( bool is_az, float* deg )
{
    bool valid;
    taskENTER_CRITICAL();
    if ( is_az ) { valid = s_manual_az_valid; if ( deg ) *deg = s_manual_az_deg; }
    else         { valid = s_manual_el_valid; if ( deg ) *deg = s_manual_el_deg; }
    taskEXIT_CRITICAL();
    return valid;
}

bool tracker_get_manual_target_info( bool is_az, float* deg, bool* absolute_ahrs )
{
    bool valid;
    taskENTER_CRITICAL();
    if ( is_az ) {
        valid = s_manual_az_valid;
        if ( deg ) *deg = s_manual_az_deg;
        if ( absolute_ahrs ) *absolute_ahrs = s_manual_az_absolute_ahrs;
    } else {
        valid = s_manual_el_valid;
        if ( deg ) *deg = s_manual_el_deg;
        if ( absolute_ahrs ) *absolute_ahrs = s_manual_el_absolute_ahrs;
    }
    taskEXIT_CRITICAL();
    return valid;
}

void tracker_clear_manual_target( bool is_az )
{
    taskENTER_CRITICAL();
    if ( is_az ) {
        s_manual_az_valid = false;
        s_manual_az_absolute_ahrs = false;
    } else {
        s_manual_el_valid = false;
        s_manual_el_absolute_ahrs = false;
    }
    taskEXIT_CRITICAL();
}

void tracker_clear_manual_targets()
{
    taskENTER_CRITICAL();
    s_manual_az_valid = false;
    s_manual_el_valid = false;
    s_manual_az_absolute_ahrs = false;
    s_manual_el_absolute_ahrs = false;
    taskEXIT_CRITICAL();
}

bool tracker_set_mode_from_proto( groundstation_TrackerMode mode )
{
    switch ( mode ) {
        case groundstation_TrackerMode_TRACKER_MODE_STOP:
            tracker_set_mode( TrackerMode::Stop );
            return true;
        case groundstation_TrackerMode_TRACKER_MODE_MANUAL:
            tracker_set_mode( TrackerMode::Manual );
            return true;
        case groundstation_TrackerMode_TRACKER_MODE_AUTO:
            tracker_set_mode( TrackerMode::Auto );
            return true;
        case groundstation_TrackerMode_TRACKER_MODE_SCAN:
            tracker_set_mode( TrackerMode::Scan );
            return true;
        case groundstation_TrackerMode_TRACKER_MODE_SERVOTEST:
            tracker_set_mode( TrackerMode::ServoTest );
            return true;
        case groundstation_TrackerMode_TRACKER_MODE_FAULT:
            tracker_set_mode( TrackerMode::Fault );
            return true;
        default:
            return false;
    }
}

bool tracker_is_armed()
{
    bool armed;
    taskENTER_CRITICAL();
    armed = s_armed;
    taskEXIT_CRITICAL();
    return armed;
}

void tracker_set_armed( bool armed )
{
    taskENTER_CRITICAL();
    s_armed = armed;
    taskEXIT_CRITICAL();
}

void tracker_set_control_status( const TrackerControlStatus& status )
{
    taskENTER_CRITICAL();
    s_control_status = status;
    taskEXIT_CRITICAL();
}

static void copy_status_text( char* dst, size_t dst_len, const char* src )
{
    if ( dst_len == 0 ) return;
    if ( !src ) src = "";
    strncpy( dst, src, dst_len - 1 );
    dst[dst_len - 1] = '\0';
}

void tracker_mark_az_calibrated( float reference_deg, const char* status )
{
    taskENTER_CRITICAL();
    s_cal_status.az_calibrated = true;
    s_cal_status.az_reference_deg = reference_deg;
    s_cal_status.seq++;
    copy_status_text( s_cal_status.status, sizeof(s_cal_status.status), status );
    taskEXIT_CRITICAL();
}

void tracker_mark_el_calibrated( float reference_deg, const char* status )
{
    taskENTER_CRITICAL();
    s_cal_status.el_calibrated = true;
    s_cal_status.el_reference_deg = reference_deg;
    s_cal_status.seq++;
    copy_status_text( s_cal_status.status, sizeof(s_cal_status.status), status );
    taskEXIT_CRITICAL();
}

void tracker_clear_calibration( const char* status )
{
    taskENTER_CRITICAL();
    s_cal_status.az_calibrated = false;
    s_cal_status.el_calibrated = false;
    s_cal_status.az_reference_deg = 0.0f;
    s_cal_status.el_reference_deg = 0.0f;
    s_cal_status.seq++;
    copy_status_text( s_cal_status.status, sizeof(s_cal_status.status), status );
    taskEXIT_CRITICAL();
}

bool tracker_axes_calibrated()
{
    bool calibrated;
    taskENTER_CRITICAL();
    calibrated = s_cal_status.az_calibrated && s_cal_status.el_calibrated;
    taskEXIT_CRITICAL();
    return calibrated;
}

bool tracker_elevation_calibrated()
{
    bool calibrated;
    taskENTER_CRITICAL();
    calibrated = s_cal_status.el_calibrated;
    taskEXIT_CRITICAL();
    return calibrated;
}
