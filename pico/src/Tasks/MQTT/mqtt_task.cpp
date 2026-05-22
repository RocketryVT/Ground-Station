#include "mqtt_task.hpp"
#include "shared.hpp"
#include "Tasks/Stepper/stepper_task.hpp"

#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/mqtt.h"

#include <stdlib.h>
#include <string.h>

static mqtt_client_t*  s_mqtt           = nullptr;
static volatile bool   s_mqtt_connected = false;
static volatile bool   s_raw_imu_enabled = false;
static volatile bool   s_raw_mag_enabled = false;
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

bool mqtt_is_connected() { return s_mqtt_connected; }
bool mqtt_raw_imu_enabled() { return s_raw_imu_enabled; }
bool mqtt_raw_mag_enabled() { return s_raw_mag_enabled; }

static bool json_bool_field( const char* json, const char* key, bool* out )
{
    const char* p = strstr( json, key );
    if ( !p ) return false;
    const char* colon = strchr( p, ':' );
    if ( !colon ) return false;
    colon++;
    while ( *colon == ' ' || *colon == '\t' ) colon++;
    if ( strncmp( colon, "true", 4 ) == 0 ) {
        *out = true;
        return true;
    }
    if ( strncmp( colon, "false", 5 ) == 0 ) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_number_field( const char* json, const char* key, float* out )
{
    const char* p = strstr( json, key );
    if ( !p ) return false;
    const char* colon = strchr( p, ':' );
    if ( !colon ) return false;
    colon++;
    while ( *colon == ' ' || *colon == '\t' ) colon++;

    char* end = nullptr;
    float value = strtof( colon, &end );
    if ( end == colon ) return false;

    *out = value;
    return true;
}

static void store_axis_cmd( const char* payload, volatile PendingAxisCmd* pending )
{
    float target = 0.0f;
    const bool has_target =
        json_number_field( payload, "\"target_angle_deg\"", &target );

    float speed = 0.0f;
    const bool has_speed =
        json_number_field( payload, "\"speed_dps\"", &speed );

    bool stop = false;
    const bool has_stop = json_bool_field( payload, "\"stop\"", &stop );

    pending->has_target       = has_target;
    pending->target_angle_deg = target;
    pending->has_speed        = has_speed;
    pending->speed_dps        = speed;
    pending->has_stop         = has_stop;
    pending->stop             = stop;
    pending->pending          = true;
}

static void store_jog_cmd( const char* payload )
{
    const bool az_axis  = strstr( payload, "\"axis\":\"az\"" ) != nullptr;
    const bool zen_axis = strstr( payload, "\"axis\":\"zen\"" ) != nullptr;
    if ( !az_axis && !zen_axis ) return;

    float delta = 0.0f;
    if ( !json_number_field( payload, "\"delta_deg\"", &delta ) ) return;

    float speed = 0.0f;
    const bool has_speed = json_number_field( payload, "\"speed_dps\"", &speed );

    s_pending_jog_cmd.axis_is_az = az_axis;
    s_pending_jog_cmd.delta_deg  = delta;
    s_pending_jog_cmd.has_speed  = has_speed;
    s_pending_jog_cmd.speed_dps  = speed;
    s_pending_jog_cmd.pending    = true;
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

    publish_stepper_cmd( q, cmd, axis );
}

static void process_pending_commands()
{
    PendingAxisCmd axis = {};
    if ( take_pending_axis_cmd( &s_pending_az_cmd, &axis ) ) {
        apply_axis_cmd( g_stepper_az_cmd_q, "az", axis );
    }
    if ( take_pending_axis_cmd( &s_pending_zen_cmd, &axis ) ) {
        apply_axis_cmd( g_stepper_zen_cmd_q, "zen", axis );
    }

    PendingJogCmd jog = {};
    if ( take_pending_jog_cmd( &jog ) ) {
        QueueHandle_t q = jog.axis_is_az ? g_stepper_az_cmd_q : g_stepper_zen_cmd_q;
        const char* axis = jog.axis_is_az ? "az" : "zen";

        StepperCmd cmd = {};
        if ( q ) xQueuePeek( q, &cmd, 0 );
        cmd.target_angle_deg += jog.delta_deg;
        cmd.stop = false;
        if ( jog.has_speed ) cmd.speed_dps = jog.speed_dps;

        publish_stepper_cmd( q, cmd, axis );
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

    char payload[128] = {};
    const u16_t copy_len = len < sizeof(payload) - 1 ? len : sizeof(payload) - 1;
    memcpy( payload, data, copy_len );

    if ( strcmp( s_in_topic, "gs/cmd/raw_sensors" ) == 0 ) {
        bool value = false;
        if ( json_bool_field( payload, "\"imu\"", &value ) )
            s_raw_imu_enabled = value;
        if ( json_bool_field( payload, "\"mag\"", &value ) )
            s_raw_mag_enabled = value;
    } else if ( strcmp( s_in_topic, "gs/cmd/az" ) == 0 ) {
        store_axis_cmd( payload, &s_pending_az_cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/zen" ) == 0 ) {
        store_axis_cmd( payload, &s_pending_zen_cmd );
    } else if ( strcmp( s_in_topic, "gs/cmd/jog" ) == 0 ) {
        store_jog_cmd( payload );
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

    struct mqtt_connect_client_info_t ci = {};
    ci.client_id  = MQTT_CLIENT_ID;
    ci.keep_alive = 60;

    cyw43_arch_lwip_begin();
    if ( !s_mqtt ) s_mqtt = mqtt_client_new();
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
        mqtt_subscribe( s_mqtt, "gs/cmd/raw_sensors", 0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/az", 0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/zen", 0, nullptr, nullptr );
        mqtt_subscribe( s_mqtt, "gs/cmd/jog", 0, nullptr, nullptr );
        cyw43_arch_lwip_end();

        // Drain the publish queue while broker is alive
        while ( s_mqtt_connected ) {
            process_pending_commands();

            if ( xQueueReceive( g_mqtt_queue, &msg, pdMS_TO_TICKS( 50 ) ) == pdTRUE ) {
                cyw43_arch_lwip_begin();
                err_t pub_err = mqtt_publish( s_mqtt,
                                               msg.topic,
                                               msg.payload,
                                               ( u16_t ) strlen( msg.payload ),
                                               0,        // QoS 0
                                               0,        // not retained
                                               nullptr, nullptr );
                cyw43_arch_lwip_end();
                if ( pub_err != ERR_OK ) {
                    log_print( "[mqtt] publish failed topic=%s len=%u err=%d\n",
                               msg.topic,
                               (unsigned)strlen( msg.payload ),
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
