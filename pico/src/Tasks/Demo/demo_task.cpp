#include "demo_task.hpp"
#include "shared.hpp"

#include <stdio.h>

static void demo_task( void* param )
{
    ( void ) param;

    // Wait for WiFi before publishing
    xEventGroupWaitBits( g_net_events, EVT_WIFI_CONNECTED,
                         pdFALSE, pdTRUE, portMAX_DELAY );

    uint32_t count = 0;

    for ( ;; ) {
        MqttMessage msg;
        uint32_t uptime_ms = ( uint32_t ) ( xTaskGetTickCount() * portTICK_PERIOD_MS );

        snprintf( msg.topic,   sizeof( msg.topic ),   "gs/demo" );
        snprintf( msg.payload, sizeof( msg.payload ),
                  "{\"count\":%lu,\"uptime_ms\":%lu}",
                  ( unsigned long ) count,
                  ( unsigned long ) uptime_ms );

        xQueueSend( g_mqtt_queue, &msg, 0 );
        ++count;

        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

static StaticTask_t s_demo_tcb;
static StackType_t  s_demo_stack[ 512 ];

void demo_task_init()
{
    task_create( demo_task, "demo", 512, nullptr, tskIDLE_PRIORITY + 1,
                  s_demo_stack, &s_demo_tcb );
}
