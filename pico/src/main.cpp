// Ground Station – SX1276 LoRa receiver + GPS + MQTT publisher
// FreeRTOS / RP2350 (Pico 2W)
//
// Task layout:
//   wifi  (pri 3) – connects to AP; sets EVT_WIFI_CONNECTED
//   mqtt  (pri 2) – waits for WiFi; connects broker; drains g_mqtt_queue
//   lora  (pri 4) – SX1276 receive; enqueues to g_mqtt_queue → rocket/lora
//   gps   (pri 3) – UART1 NMEA parse; enqueues to g_mqtt_queue → rocket/gps
//   servo (pri 2) – reads location queues; drives azimuth + elevation servos
//   usb   (pri 1) – USB CDC status logger
//
// All FreeRTOS objects (tasks, queues, event groups, idle/timer tasks) are
// statically allocated — no heap usage for scheduler infrastructure.

#include "shared.hpp"

#include "Tasks/WiFi/wifi_task.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Tasks/LoRa/lora_task.hpp"
#include "Tasks/GPS/gps_task.hpp"
#include "Tasks/Servo/servo_task.hpp"
#include "Tasks/USB/usb_task.hpp"

#include "pico/stdlib.h"

#include <stdio.h>

// ── Shared FreeRTOS handles (declared extern in shared.hpp) ───────────────────
EventGroupHandle_t g_net_events        = nullptr;
QueueHandle_t      g_mqtt_queue        = nullptr;
QueueHandle_t      g_log_queue         = nullptr;
QueueHandle_t      g_gs_location_q     = nullptr;
QueueHandle_t      g_rocket_location_q = nullptr;

// ── Static backing storage for queues and event group ────────────────────────
static StaticEventGroup_t s_net_events_buf;

static StaticQueue_t s_mqtt_queue_buf;
static uint8_t       s_mqtt_queue_storage[ MQTT_QUEUE_DEPTH * sizeof( MqttMessage ) ];

static StaticQueue_t s_log_queue_buf;
static uint8_t       s_log_queue_storage [ LOG_QUEUE_DEPTH  * sizeof( LogMessage   ) ];

// Depth-1 overwrite queues — always hold the latest known location.
static StaticQueue_t s_gs_location_buf;
static uint8_t       s_gs_location_storage    [ sizeof( LocationMsg ) ];

static StaticQueue_t s_rocket_location_buf;
static uint8_t       s_rocket_location_storage[ sizeof( LocationMsg ) ];

// ── FreeRTOS static-allocation callbacks ──────────────────────────────────────
// Required whenever configSUPPORT_STATIC_ALLOCATION == 1.
// FreeRTOS calls these to obtain memory for the Idle and Timer daemon tasks,
// which it creates internally before vTaskStartScheduler returns control.

extern "C" {

void vApplicationGetIdleTaskMemory( StaticTask_t**  ppxIdleTaskTCBBuffer,
                                     StackType_t**   ppxIdleTaskStackBuffer,
                                     uint32_t*       pulIdleTaskStackSize )
{
    static StaticTask_t idle_tcb;
    static StackType_t  idle_stack[ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer   = &idle_tcb;
    *ppxIdleTaskStackBuffer =  idle_stack;
    *pulIdleTaskStackSize   =  configMINIMAL_STACK_SIZE;
}

// RP2350 has 2 cores — FreeRTOS SMP creates one passive idle task per
// additional core (index 0 = core 1's idle task).
void vApplicationGetPassiveIdleTaskMemory( StaticTask_t**  ppxIdleTaskTCBBuffer,
                                            StackType_t**   ppxIdleTaskStackBuffer,
                                            uint32_t*       pulIdleTaskStackSize,
                                            BaseType_t      xPassiveIdleTaskIndex )
{
    static StaticTask_t passive_tcb  [ configNUMBER_OF_CORES - 1 ];
    static StackType_t  passive_stack[ configNUMBER_OF_CORES - 1 ]
                                     [ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer   = &passive_tcb  [ xPassiveIdleTaskIndex ];
    *ppxIdleTaskStackBuffer =  passive_stack [ xPassiveIdleTaskIndex ];
    *pulIdleTaskStackSize   =  configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t**  ppxTimerTaskTCBBuffer,
                                      StackType_t**   ppxTimerTaskStackBuffer,
                                      uint32_t*       pulTimerTaskStackSize )
{
    static StaticTask_t timer_tcb;
    static StackType_t  timer_stack[ configTIMER_TASK_STACK_DEPTH ];

    *ppxTimerTaskTCBBuffer   = &timer_tcb;
    *ppxTimerTaskStackBuffer =  timer_stack;
    *pulTimerTaskStackSize   =  configTIMER_TASK_STACK_DEPTH;
}

} // extern "C"

// ── Entry point ───────────────────────────────────────────────────────────────
int main( void )
{
    stdio_init_all();

    // Wait for USB CDC to enumerate so the first log lines are not lost
    while ( !stdio_usb_connected() ) {
        sleep_ms( 100 );
    }
    sleep_ms( 500 );

    printf( "=== Ground Station — LoRa + GPS + MQTT ===\n" );
    printf( "    Board : Pico 2W (RP2350)\n" );
    printf( "    LoRa  : SX1276  GPIO NSS=%u DIO0=%u RST=%u\n",
            Pins::LORA_NSS, Pins::LORA_DIO0, Pins::LORA_RST );
    printf( "    GPS   : UART1   GPIO RX=%u  TX=%u\n",
            Pins::GPS_UART_RX, Pins::GPS_UART_TX );
    printf( "    MQTT  : %s:%d  client=%s\n\n",
            MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_CLIENT_ID );

    // Create shared synchronisation objects (statically backed)
    g_net_events = xEventGroupCreateStatic( &s_net_events_buf );

    g_mqtt_queue = xQueueCreateStatic( MQTT_QUEUE_DEPTH,
                                        sizeof( MqttMessage ),
                                        s_mqtt_queue_storage,
                                        &s_mqtt_queue_buf );
    g_log_queue  = xQueueCreateStatic( LOG_QUEUE_DEPTH,
                                        sizeof( LogMessage ),
                                        s_log_queue_storage,
                                        &s_log_queue_buf );

    g_gs_location_q     = xQueueCreateStatic( 1, sizeof( LocationMsg ),
                                               s_gs_location_storage,
                                               &s_gs_location_buf );
    g_rocket_location_q = xQueueCreateStatic( 1, sizeof( LocationMsg ),
                                               s_rocket_location_storage,
                                               &s_rocket_location_buf );

    // Spawn tasks (each allocates its own static TCB + stack internally)
    wifi_task_init();
    mqtt_task_init();
    lora_task_init();
    gps_task_init();
    servo_task_init();
    usb_task_init();

    vTaskStartScheduler();

    for ( ;; ) {}
}
