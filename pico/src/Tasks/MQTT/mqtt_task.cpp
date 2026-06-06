#include "mqtt_task.hpp"
#include "shared.hpp"
#include "Tasks/Stepper/stepper_task.hpp"
#include "Tasks/Stepper/tracker_state.hpp"
#include "Tasks/Fusion/fusion_task.hpp"
#include "Proto/ground_station.pb.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/mqtt.h"
#include "pb_decode.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static constexpr uint32_t kCalibrationAhrsMaxAgeMs = 500;

static float wrap_180( float deg )
{
    while ( deg > 180.0f )   deg -= 360.0f;
    while ( deg <= -180.0f ) deg += 360.0f;
    return deg;
}

static mqtt_client_t*  s_mqtt           = nullptr;
static volatile bool   s_mqtt_connected = false;
static volatile bool   s_raw_imu_enabled     = false;
static volatile bool   s_raw_mag_enabled     = false;
static volatile bool   s_raw_yaw_imu_enabled = false;
static char            s_in_topic[64] = {};

struct PendingAxisCmd {
    bool  pending;
    bool  has_target;
    float target_angle_deg;
    bool  has_speed;
    float speed_dps;
    bool  has_stop;
    bool  stop;
};

struct PendingJogCmd {
    bool  pending;
    bool  axis_is_az;
    float delta_deg;
    bool  has_speed;
    float speed_dps;
};

static volatile PendingAxisCmd s_pending_az_cmd  = {};
static volatile PendingAxisCmd s_pending_zen_cmd = {};
static volatile PendingJogCmd  s_pending_jog_cmd = {};

struct PendingModeCmd {
    bool pending;
    groundstation_TrackerMode mode;
};

struct PendingArmCmd {
    bool pending;
    bool armed;
};

struct PendingConfigCmd {
    bool pending;
    groundstation_TrackerConfigCommand cfg;
};

struct PendingLocationCmd {
    bool pending;
    bool is_ground_station;
    double lat;
    double lon;
    double alt_m;
};

struct PendingCalibrationCmd {
    bool pending;
    groundstation_CalibrationCommand cmd;
};

static PendingModeCmd     s_pending_mode_cmd = {};
static PendingArmCmd      s_pending_arm_cmd = {};
static PendingConfigCmd   s_pending_config_cmd = {};
static PendingLocationCmd s_pending_location_cmd = {};
static PendingCalibrationCmd s_pending_calibration_cmd = {};

bool mqtt_is_connected() { return s_mqtt_connected; }
bool mqtt_raw_imu_enabled()     { return s_raw_imu_enabled; }
bool mqtt_raw_mag_enabled()     { return s_raw_mag_enabled; }
bool mqtt_raw_yaw_imu_enabled() { return s_raw_yaw_imu_enabled; }

static bool decode_proto( const u8_t* data,
                          u16_t len,
                          const pb_msgdesc_t* fields,
                          void* out )
{
    pb_istream_t stream = pb_istream_from_buffer( data, len );
    return pb_decode( &stream, fields, out );
}

static void store_axis_cmd( const groundstation_AxisCommand& cmd,
                            volatile PendingAxisCmd* pending )
{
    pending->has_target       = cmd.has_target_angle_deg;
    pending->target_angle_deg = cmd.target_angle_deg;
    pending->has_speed        = cmd.has_speed_dps;
    pending->speed_dps        = cmd.speed_dps;
    pending->has_stop         = cmd.has_stop;
    pending->stop             = cmd.stop;
    pending->pending          = true;
}

static void store_jog_cmd( const groundstation_JogCommand& cmd )
{
    if ( !cmd.has_axis || !cmd.has_delta_deg ) return;

    const bool az_axis = cmd.axis == groundstation_JogAxis_JOG_AXIS_AZ;
    const bool el_axis = cmd.axis == groundstation_JogAxis_JOG_AXIS_EL;
    if ( !az_axis && !el_axis ) return;

    s_pending_jog_cmd.axis_is_az = az_axis;
    s_pending_jog_cmd.delta_deg  = cmd.delta_deg;
    s_pending_jog_cmd.has_speed  = cmd.has_speed_dps;
    s_pending_jog_cmd.speed_dps  = cmd.speed_dps;
    s_pending_jog_cmd.pending    = true;
}

static void store_mode_cmd( const groundstation_TrackerModeCommand& cmd )
{
    if ( !cmd.has_mode ) return;
    s_pending_mode_cmd.mode = cmd.mode;
    s_pending_mode_cmd.pending = true;
}

static void store_arm_cmd( const groundstation_TrackerArmCommand& cmd )
{
    if ( !cmd.has_armed ) return;
    s_pending_arm_cmd.armed = cmd.armed;
    s_pending_arm_cmd.pending = true;
}

static void store_config_cmd( const groundstation_TrackerConfigCommand& cmd )
{
    s_pending_config_cmd.cfg = cmd;
    s_pending_config_cmd.pending = true;
}

static void store_calibration_cmd( const groundstation_CalibrationCommand& cmd )
{
    if ( !cmd.has_action ) return;
    s_pending_calibration_cmd.cmd = cmd;
    s_pending_calibration_cmd.pending = true;
}

static void store_location_cmd( const groundstation_LocationCommand& cmd,
                                bool is_ground_station )
{
    if ( !cmd.has_lat || !cmd.has_lon || !cmd.has_alt_m ) return;
    s_pending_location_cmd.is_ground_station = is_ground_station;
    s_pending_location_cmd.lat = cmd.lat;
    s_pending_location_cmd.lon = cmd.lon;
    s_pending_location_cmd.alt_m = cmd.alt_m;
    s_pending_location_cmd.pending = true;
}

static bool publish_stepper_cmd( QueueHandle_t q,
                                 const StepperCmd& cmd,
                                 const char* axis )
{
    if ( !q ) {
        log_print( "[mqtt] %s command dropped: stepper queue not ready\n", axis );
        return false;
    }

    xQueueOverwrite( q, &cmd );
    log_print( "[mqtt] %s cmd target=%.2f speed=%.2f stop=%s\n",
               axis,
               (double)cmd.target_angle_deg,
               (double)cmd.speed_dps,
               cmd.stop ? "true" : "false" );
    return true;
}

static void publish_calibration_event( const char* action,
                                       const char* result,
                                       float reference_deg,
                                       const char* note )
{
    MqttMessage msg = {};
    strncpy( msg.topic, "antenna/calibration/event", sizeof(msg.topic) - 1 );

    const TrackerCalibrationStatus cal = tracker_calibration_status_snapshot();
    char safe_note[96] = {};
    if ( note ) {
        size_t out = 0;
        for ( size_t in = 0; note[in] != '\0' && out < sizeof(safe_note) - 1; in++ ) {
            const char c = note[in];
            safe_note[out++] = ( c == '"' || c == '\\' ) ? '\'' : c;
        }
    }
    const int n = snprintf( (char*)msg.payload, sizeof(msg.payload),
        "{\"timestamp\":%llu,\"seq\":%lu,\"action\":\"%s\",\"result\":\"%s\","
        "\"reference_deg\":%.3f,\"az_calibrated\":%s,\"el_calibrated\":%s,"
        "\"az_reference_deg\":%.3f,\"el_reference_deg\":%.3f,\"note\":\"%s\"}",
        (unsigned long long)( time_us_64() / 1000u ),
        (unsigned long)cal.seq,
        action ? action : "",
        result ? result : "",
        (double)reference_deg,
        cal.az_calibrated ? "true" : "false",
        cal.el_calibrated ? "true" : "false",
        (double)cal.az_reference_deg,
        (double)cal.el_reference_deg,
        safe_note );

    if ( n > 0 ) {
        msg.payload_len = (uint16_t)( n < (int)sizeof(msg.payload)
            ? n
            : (int)sizeof(msg.payload) - 1 );
        xQueueSend( g_mqtt_queue, &msg, 0 );
    }
}

static bool apply_yaw_heading_reference( float reference_deg,
                                         float* delta_deg,
                                         float* total_offset_deg )
{
    if ( delta_deg ) *delta_deg = 0.0f;
    if ( total_offset_deg ) *total_offset_deg = fusion_get_heading_offset();

    if ( !g_imu_q ) return false;

    ImuMsg imu = {};
    if ( xQueuePeek( g_imu_q, &imu, 0 ) != pdTRUE ||
         !imu.valid ||
         !imu.have_yaw_frame ) {
        return false;
    }

    const uint64_t now_us = time_us_64();
    if ( imu.timestamp_us == 0 ||
         now_us - imu.timestamp_us > (uint64_t)kCalibrationAhrsMaxAgeMs * 1000ull ) {
        return false;
    }

    const float delta = wrap_180( reference_deg - imu.yaw_frame_yaw360 );
    fusion_adjust_heading_offset( delta );

    if ( delta_deg ) *delta_deg = delta;
    if ( total_offset_deg ) *total_offset_deg = fusion_get_heading_offset();
    return true;
}

static bool take_pending_axis_cmd( volatile PendingAxisCmd* pending,
                                   PendingAxisCmd* out )
{
    bool have = false;

    taskENTER_CRITICAL();
    if ( pending->pending ) {
        out->has_target       = pending->has_target;
        out->target_angle_deg = pending->target_angle_deg;
        out->has_speed        = pending->has_speed;
        out->speed_dps        = pending->speed_dps;
        out->has_stop         = pending->has_stop;
        out->stop             = pending->stop;
        pending->pending      = false;
        have = true;
    }
    taskEXIT_CRITICAL();

    return have;
}

static bool take_pending_jog_cmd( PendingJogCmd* out )
{
    bool have = false;

    taskENTER_CRITICAL();
    if ( s_pending_jog_cmd.pending ) {
        out->axis_is_az          = s_pending_jog_cmd.axis_is_az;
        out->delta_deg           = s_pending_jog_cmd.delta_deg;
        out->has_speed           = s_pending_jog_cmd.has_speed;
        out->speed_dps           = s_pending_jog_cmd.speed_dps;
        s_pending_jog_cmd.pending = false;
        have = true;
    }
    taskEXIT_CRITICAL();

    return have;
}

static void apply_axis_cmd( QueueHandle_t q,
                            const char* axis,
                            const PendingAxisCmd& pending )
{
    StepperCmd cmd = {};
    if ( q ) xQueuePeek( q, &cmd, 0 );

    if ( pending.has_target ) cmd.target_angle_deg = pending.target_angle_deg;
    if ( pending.has_speed )  cmd.speed_dps        = pending.speed_dps;
    if ( pending.has_stop )   cmd.stop             = pending.stop;

    if ( !cmd.stop && !tracker_is_armed() ) {
        log_print( "[mqtt] %s command dropped: tracker disarmed\n", axis );
        return;
    }
    if ( !cmd.stop ) {
        tracker_set_mode( TrackerMode::Manual );
    }
    publish_stepper_cmd( q, cmd, axis );
}

static void process_pending_commands()
{
    PendingAxisCmd axis = {};
    if ( take_pending_axis_cmd( &s_pending_az_cmd, &axis ) ) {
        apply_axis_cmd( g_stepper_az_cmd_q, "az", axis );
    }
    if ( take_pending_axis_cmd( &s_pending_zen_cmd, &axis ) ) {
        apply_axis_cmd( g_stepper_zen_cmd_q, "el", axis );
    }

    PendingJogCmd jog = {};
    if ( take_pending_jog_cmd( &jog ) ) {
        QueueHandle_t q = jog.axis_is_az ? g_stepper_az_cmd_q : g_stepper_zen_cmd_q;
        const char* axis = jog.axis_is_az ? "az" : "el";

        StepperCmd cmd = {};
        if ( q ) xQueuePeek( q, &cmd, 0 );
        cmd.target_angle_deg += jog.delta_deg;
        cmd.stop = false;
        if ( jog.has_speed ) cmd.speed_dps = jog.speed_dps;

        if ( !tracker_is_armed() ) {
            log_print( "[mqtt] %s jog dropped: tracker disarmed\n", axis );
        } else {
            tracker_set_mode( TrackerMode::Manual );
            publish_stepper_cmd( q, cmd, axis );
        }
    }

    taskENTER_CRITICAL();
    PendingModeCmd mode_cmd = {};
    if ( s_pending_mode_cmd.pending ) {
        mode_cmd.mode = s_pending_mode_cmd.mode;
        mode_cmd.pending = true;
        s_pending_mode_cmd.pending = false;
    }
    PendingArmCmd arm_cmd = {};
    if ( s_pending_arm_cmd.pending ) {
        arm_cmd.armed = s_pending_arm_cmd.armed;
        arm_cmd.pending = true;
        s_pending_arm_cmd.pending = false;
    }
    PendingConfigCmd config_cmd = {};
    if ( s_pending_config_cmd.pending ) {
        config_cmd.cfg = s_pending_config_cmd.cfg;
        config_cmd.pending = true;
        s_pending_config_cmd.pending = false;
    }
    PendingLocationCmd loc_cmd = {};
    if ( s_pending_location_cmd.pending ) {
        loc_cmd.is_ground_station = s_pending_location_cmd.is_ground_station;
        loc_cmd.lat = s_pending_location_cmd.lat;
        loc_cmd.lon = s_pending_location_cmd.lon;
        loc_cmd.alt_m = s_pending_location_cmd.alt_m;
        loc_cmd.pending = true;
        s_pending_location_cmd.pending = false;
    }
    PendingCalibrationCmd cal_cmd = {};
    if ( s_pending_calibration_cmd.pending ) {
        cal_cmd.cmd = s_pending_calibration_cmd.cmd;
        cal_cmd.pending = true;
        s_pending_calibration_cmd.pending = false;
    }
    taskEXIT_CRITICAL();

    if ( mode_cmd.pending ) {
        if ( tracker_set_mode_from_proto( mode_cmd.mode ) ) {
            log_print( "[mqtt] tracker mode=%s\n", tracker_mode_name( tracker_mode() ) );
        }
    }
    if ( arm_cmd.pending ) {
        tracker_set_armed( arm_cmd.armed );
        log_print( "[mqtt] tracker armed=%u\n", arm_cmd.armed ? 1u : 0u );
    }
    if ( config_cmd.pending ) {
        tracker_apply_config_command( config_cmd.cfg );
        log_print( "[mqtt] tracker config updated\n" );
    }
    if ( loc_cmd.pending ) {
        LocationMsg loc {
            loc_cmd.lat,
            loc_cmd.lon,
            loc_cmd.alt_m,
            time_us_64()
        };
        QueueHandle_t q = loc_cmd.is_ground_station ? g_gs_location_q : g_rocket_location_q;
        if ( q ) {
            xQueueOverwrite( q, &loc );
            log_print( "[mqtt] %s location lat=%.7f lon=%.7f alt=%.1f\n",
                       loc_cmd.is_ground_station ? "gs" : "rocket",
                       loc_cmd.lat, loc_cmd.lon, loc_cmd.alt_m );
        }
    }
    if ( cal_cmd.pending ) {
        const groundstation_CalibrationCommand& cmd = cal_cmd.cmd;
        const float ref = cmd.has_reference_deg ? cmd.reference_deg : 0.0f;
        const char* note = cmd.has_note ? cmd.note : "";

        switch ( cmd.action ) {
        case groundstation_CalibrationAction_CAL_ACTION_BEGIN_GUIDED:
            tracker_set_mode( TrackerMode::Manual );
            tracker_set_armed( true );
            tracker_clear_calibration( "guided calibration started" );
            fusion_adjust_heading_offset( -fusion_get_heading_offset() );
            publish_calibration_event( "begin_guided", "ok", ref, note );
            log_print( "[cal] guided calibration started\n" );
            break;

        case groundstation_CalibrationAction_CAL_ACTION_SET_AZ_REFERENCE:
            if ( cmd.has_reference_deg && g_stepper_az_cal_q ) {
                float yaw_delta = 0.0f;
                float yaw_offset = 0.0f;
                const bool yaw_adjusted =
                    apply_yaw_heading_reference( ref, &yaw_delta, &yaw_offset );

                StepperCalibrationCmd axis_cal = {};
                axis_cal.set_current_angle = true;
                axis_cal.current_angle_deg = ref;
                xQueueOverwrite( g_stepper_az_cal_q, &axis_cal );
                publish_calibration_event( "set_az_reference", "ok", ref, note );
                if ( yaw_adjusted ) {
                    log_print( "[cal] az reference %.2f; yaw offset delta=%.2f total=%.2f\n",
                               (double)ref, (double)yaw_delta, (double)yaw_offset );
                } else {
                    log_print( "[cal] az reference %.2f; yaw AHRS offset unchanged\n",
                               (double)ref );
                }
            } else {
                publish_calibration_event( "set_az_reference", "rejected", ref, note );
            }
            break;

        case groundstation_CalibrationAction_CAL_ACTION_SET_EL_REFERENCE:
            if ( cmd.has_reference_deg && g_stepper_zen_cal_q ) {
                StepperCalibrationCmd axis_cal = {};
                axis_cal.set_current_angle = true;
                axis_cal.current_angle_deg = ref;
                xQueueOverwrite( g_stepper_zen_cal_q, &axis_cal );
                publish_calibration_event( "set_el_reference", "ok", ref, note );
            } else {
                publish_calibration_event( "set_el_reference", "rejected", ref, note );
            }
            break;

        case groundstation_CalibrationAction_CAL_ACTION_CLEAR:
        {
            StepperCalibrationCmd axis_cal = {};
            axis_cal.clear = true;
            if ( g_stepper_az_cal_q ) xQueueOverwrite( g_stepper_az_cal_q, &axis_cal );
            if ( g_stepper_zen_cal_q ) xQueueOverwrite( g_stepper_zen_cal_q, &axis_cal );
            tracker_clear_calibration( "calibration cleared" );
            fusion_adjust_heading_offset( -fusion_get_heading_offset() );
            tracker_set_mode( TrackerMode::Manual );
            tracker_set_armed( false );
            publish_calibration_event( "clear", "ok", ref, note );
            log_print( "[cal] calibration cleared\n" );
            break;
        }

        case groundstation_CalibrationAction_CAL_ACTION_ENABLE_TRACKING:
            if ( tracker_axes_calibrated() ) {
                tracker_set_mode( TrackerMode::Auto );
                tracker_set_armed( true );
                publish_calibration_event( "enable_tracking", "ok", ref, note );
                log_print( "[cal] tracking enabled after calibration\n" );
            } else {
                tracker_set_mode( TrackerMode::Manual );
                publish_calibration_event( "enable_tracking", "rejected", ref, note );
                log_print( "[cal] tracking enable blocked: axes not calibrated\n" );
            }
            break;

        default:
            publish_calibration_event( "unknown", "rejected", ref, note );
            break;
        }
    }
}

static void incoming_publish_cb( void* arg, const char* topic, u32_t tot_len )
{
    ( void ) arg; ( void ) tot_len;
    strncpy( s_in_topic, topic, sizeof(s_in_topic) - 1 );
    s_in_topic[ sizeof(s_in_topic) - 1 ] = '\0';
}

static void incoming_data_cb( void* arg, const u8_t* data, u16_t len, u8_t flags )
{
    ( void ) arg;
    if ( !( flags & MQTT_DATA_FLAG_LAST ) ) return;

    if ( strcmp( s_in_topic, "gs/cmd/raw_sensors" ) == 0 ) {
        groundstation_RawSensorsCommand cmd = groundstation_RawSensorsCommand_init_zero;
        if ( decode_proto( data, len, groundstation_RawSensorsCommand_fields, &cmd ) ) {
            if ( cmd.has_imu ) s_raw_imu_enabled = cmd.imu;
            if ( cmd.has_mag ) s_raw_mag_enabled = cmd.mag;
            if ( cmd.has_yaw_imu ) s_raw_yaw_imu_enabled = cmd.yaw_imu;
        }
    } else if ( strcmp( s_in_topic, "gs/cmd/declination" ) == 0 ) {
        groundstation_DeclinationCommand cmd = groundstation_DeclinationCommand_init_zero;
        if ( decode_proto( data, len, groundstation_DeclinationCommand_fields, &cmd )
             && cmd.has_declination_deg )
        {
            fusion_set_declination( cmd.declination_deg );
            log_print( "[mqtt] declination set to %.2f deg\n",
                       (double)cmd.declination_deg );
        }
    } else if ( strcmp( s_in_topic, "gs/cmd/az" ) == 0 ) {
        groundstation_AxisCommand cmd = groundstation_AxisCommand_init_zero;
        if ( decode_proto( data, len, groundstation_AxisCommand_fields, &cmd ) )
            store_axis_cmd( cmd, &s_pending_az_cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/zen" ) == 0 ) {
        groundstation_AxisCommand cmd = groundstation_AxisCommand_init_zero;
        if ( decode_proto( data, len, groundstation_AxisCommand_fields, &cmd ) )
            store_axis_cmd( cmd, &s_pending_zen_cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/jog" ) == 0 ) {
        groundstation_JogCommand cmd = groundstation_JogCommand_init_zero;
        if ( decode_proto( data, len, groundstation_JogCommand_fields, &cmd ) )
            store_jog_cmd( cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/tracker/mode" ) == 0 ) {
        groundstation_TrackerModeCommand cmd = groundstation_TrackerModeCommand_init_zero;
        if ( decode_proto( data, len, groundstation_TrackerModeCommand_fields, &cmd ) )
            store_mode_cmd( cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/tracker/arm" ) == 0 ) {
        groundstation_TrackerArmCommand cmd = groundstation_TrackerArmCommand_init_zero;
        if ( decode_proto( data, len, groundstation_TrackerArmCommand_fields, &cmd ) )
            store_arm_cmd( cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/tracker/config" ) == 0 ) {
        groundstation_TrackerConfigCommand cmd = groundstation_TrackerConfigCommand_init_zero;
        if ( decode_proto( data, len, groundstation_TrackerConfigCommand_fields, &cmd ) )
            store_config_cmd( cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/calibration" ) == 0 ) {
        groundstation_CalibrationCommand cmd = groundstation_CalibrationCommand_init_zero;
        if ( decode_proto( data, len, groundstation_CalibrationCommand_fields, &cmd ) )
            store_calibration_cmd( cmd );
    } else if ( strcmp( s_in_topic, "gs/location" ) == 0 ) {
        groundstation_LocationCommand cmd = groundstation_LocationCommand_init_zero;
        if ( decode_proto( data, len, groundstation_LocationCommand_fields, &cmd ) )
            store_location_cmd( cmd, true );
    } else if ( strcmp( s_in_topic, "rocket/location" ) == 0 ) {
        groundstation_LocationCommand cmd = groundstation_LocationCommand_init_zero;
        if ( decode_proto( data, len, groundstation_LocationCommand_fields, &cmd ) )
            store_location_cmd( cmd, false );
    }
}

// -- Connection callback (fires in lwIP IRQ context) ---------------------------
// Do NOT call log_print() here — xQueueSend cannot be called from ISR.
// The MQTT task logs the outcome after the handshake delay instead.
static void connection_cb( mqtt_client_t* client, void* arg,
                            mqtt_connection_status_t status )
{
    ( void ) client; ( void ) arg;
    s_mqtt_connected = ( status == MQTT_CONNECT_ACCEPTED );
}

static bool broker_connect()
{
    ip_addr_t broker;
    if ( !ipaddr_aton( MQTT_BROKER_HOST, &broker ) ) {
        log_print( "[mqtt] invalid broker IP: %s\n", MQTT_BROKER_HOST );
        return false;
    }

    // Always tear down the previous client so its PCB is fully closed before
    // we attempt a new TCP connection.  Reusing a stale client causes
    // mqtt_client_connect() to return ERR_CLSD (-10) on every retry.
    cyw43_arch_lwip_begin();
    if ( s_mqtt ) {
        mqtt_disconnect( s_mqtt );
        mqtt_client_free( s_mqtt );
        s_mqtt = nullptr;
    }
    s_mqtt = mqtt_client_new();
    cyw43_arch_lwip_end();

    if ( !s_mqtt ) {
        log_print( "[mqtt] client alloc failed\n" );
        return false;
    }

    s_mqtt_connected = false;

    struct mqtt_connect_client_info_t ci = {};
    ci.client_id  = MQTT_CLIENT_ID;
    ci.keep_alive = 60;

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect( s_mqtt, &broker, MQTT_BROKER_PORT,
                                      connection_cb, nullptr, &ci );
    cyw43_arch_lwip_end();

    if ( err != ERR_OK ) {
        log_print( "[mqtt] connect error %d\n", err );
        return false;
    }
    return true;
}

// -- Main task -----------------------------------------------------------------
static void mqtt_task( void* param )
{
    ( void ) param;

    MqttMessage msg;

    for ( ;; ) {
        // Block until WiFi is up
        xEventGroupWaitBits( g_net_events, EVT_WIFI_CONNECTED,
                             pdFALSE, pdTRUE, portMAX_DELAY );

        log_print( "[mqtt] connecting to %s:%d...\n",
                   MQTT_BROKER_HOST, MQTT_BROKER_PORT );

        if ( !broker_connect() ) {
            vTaskDelay( pdMS_TO_TICKS( 5000 ) );
            continue;
        }

        // Allow the async TCP/MQTT handshake to complete, then check outcome
        vTaskDelay( pdMS_TO_TICKS( 2000 ) );

        if ( !s_mqtt_connected ) {
            log_print( "[mqtt] handshake timed out — retrying\n" );
            vTaskDelay( pdMS_TO_TICKS( 3000 ) );
            continue;
        }

        log_print( "[mqtt] connected to %s\n", MQTT_BROKER_HOST );

        cyw43_arch_lwip_begin();
        mqtt_set_inpub_callback( s_mqtt, incoming_publish_cb, incoming_data_cb, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/raw_sensors",  0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/az",           0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/zen",          0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/jog",          0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/declination",  0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/tracker/mode", 0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/tracker/arm",  0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/tracker/config", 0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/calibration",    0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/location",         0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "rocket/location",     0, nullptr, nullptr );
        cyw43_arch_lwip_end();

        // Drain the publish queue while broker is alive
        while ( s_mqtt_connected ) {
            process_pending_commands();

            if ( xQueueReceive( g_mqtt_queue, &msg, pdMS_TO_TICKS( 50 ) ) == pdTRUE ) {
                cyw43_arch_lwip_begin();
                err_t pub_err = mqtt_publish( s_mqtt,
                                               msg.topic,
                                               msg.payload,
                                               msg.payload_len,
                                               0,        // QoS 0
                                               0,        // not retained
                                               nullptr, nullptr );
                cyw43_arch_lwip_end();
                if ( pub_err != ERR_OK ) {
                    log_print( "[mqtt] publish failed topic=%s len=%u err=%d\n",
                               msg.topic,
                               (unsigned)msg.payload_len,
                               (int)pub_err );
                }
            }
        }

        log_print( "[mqtt] broker lost — retrying in 3 s\n" );
        vTaskDelay( pdMS_TO_TICKS( 3000 ) );
    }
}

static StaticTask_t s_mqtt_tcb;
static StackType_t  s_mqtt_stack[ 1024 ];

void mqtt_task_init()
{
    task_create( mqtt_task, "mqtt", 1024, nullptr, tskIDLE_PRIORITY + 2,
                  s_mqtt_stack, &s_mqtt_tcb );
}
