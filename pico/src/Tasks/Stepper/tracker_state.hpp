#pragma once

#include "Proto/ground_station.pb.h"

#include <stdint.h>

enum class TrackerMode : uint8_t {
    Stop = 0,
    Manual,
    Auto,
    Scan,
    ServoTest,
    Fault,
};

struct TrackerConfig {
    float yaw_trim_deg = 0.0f;
    float el_trim_deg = 0.0f;

    float az_min_deg = 0.0f;
    float az_max_deg = 360.0f;
    float el_min_deg = -10.0f;
    float el_max_deg = 90.0f;

    float default_speed_dps = 30.0f;
    float max_speed_dps = 90.0f;

    float scan_speed_az_dps = 20.0f;
    float scan_speed_el_dps = 5.0f;

    uint32_t gs_timeout_ms = 30000;
    uint32_t target_timeout_ms = 5000;
    float distance_min_m = 3.0f;
    bool scan_on_loss = true;

    bool use_ahrs_el = true;
    bool use_ahrs_az = false;
    float ahrs_max_age_ms = 250.0f;
    float ahrs_feedback_gain = 0.35f;
    float ahrs_max_correction_deg = 8.0f;
};

struct TrackerControlStatus {
    bool gs_fresh = false;
    bool target_fresh = false;
    bool ahrs_el_used = false;
    bool ahrs_az_used = false;
    float distance_m = 0.0f;
    float pointing_error_az = 0.0f;
    float pointing_error_el = 0.0f;
};

struct TrackerCalibrationStatus {
    bool az_calibrated = false;
    bool el_calibrated = false;
    float az_reference_deg = 0.0f;
    float el_reference_deg = 0.0f;
    uint32_t seq = 0;
    char status[48] = "uncalibrated";
};

TrackerConfig tracker_config_snapshot();
TrackerControlStatus tracker_control_status_snapshot();
TrackerCalibrationStatus tracker_calibration_status_snapshot();

void tracker_apply_config_command( const groundstation_TrackerConfigCommand& cmd );

TrackerMode tracker_mode();
const char* tracker_mode_name( TrackerMode mode );
void tracker_set_mode( TrackerMode mode );
bool tracker_set_mode_from_proto( groundstation_TrackerMode mode );

bool tracker_is_armed();
void tracker_set_armed( bool armed );

void tracker_set_control_status( const TrackerControlStatus& status );

// Last operator-commanded manual target per axis.  Recorded when a manual az/el
// (or jog) command is accepted so the controller can hold/servo to it via AHRS
// while in Manual mode.  Cleared automatically when leaving Manual mode.
void tracker_set_manual_target( bool is_az, float deg );
bool tracker_get_manual_target( bool is_az, float* deg );
void tracker_clear_manual_targets();

void tracker_mark_az_calibrated( float reference_deg, const char* status );
void tracker_mark_el_calibrated( float reference_deg, const char* status );
void tracker_clear_calibration( const char* status );
bool tracker_axes_calibrated();
