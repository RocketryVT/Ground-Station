#include "mqtt_task.hpp"
#include "shared.hpp"

#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/mqtt.h"

#include <string.h>

static mqtt_client_t*  s_mqtt           = nullptr;
static volatile bool   s_mqtt_connected = false;

bool mqtt_is_connected() { return s_mqtt_connected; }

// ── Connection callback (fires in lwIP IRQ context) ───────────────────────────
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

// ── Main task ─────────────────────────────────────────────────────────────────
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

        // Drain the publish queue while broker is alive
        while ( s_mqtt_connected ) {
            if ( xQueueReceive( g_mqtt_queue, &msg, pdMS_TO_TICKS( 500 ) ) == pdTRUE ) {
                cyw43_arch_lwip_begin();
                mqtt_publish( s_mqtt,
                              msg.topic,
                              msg.payload,
                              ( u16_t ) strlen( msg.payload ),
                              0,        // QoS 0
                              0,        // not retained
                              nullptr, nullptr );
                cyw43_arch_lwip_end();
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
    configASSERT( xTaskCreateStatic( mqtt_task, "mqtt", 1024,
                                      NULL, tskIDLE_PRIORITY + 2,
                                      s_mqtt_stack, &s_mqtt_tcb ) );
}
