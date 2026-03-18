// Ground Station — dual LoRa receiver + servo antenna tracker + I2C bus tasks
// FreeRTOS / RP2350 (Pico 2W)
//
// Task layout:
//   wifi      (pri 3) – connects to Starlink AP; sets EVT_WIFI_CONNECTED
//   mqtt      (pri 2) – broker connection; drains g_mqtt_queue; TODO: subscribe gs/gps
//   lora1     (pri 4) – SX1276 / 915 MHz receive → g_rocket_location_q
//   lora2     (pri 4) – RFM69HCW / 433 MHz receive → mqtt (rocket/rf69)
//   gps       (pri 1) – placeholder: GPS from Starlink via MQTT (see gps_task.cpp)
//   i2c0      (pri 2) – I2C0 bus task (barometer – not yet populated)
//   i2c1      (pri 2) – I2C1 bus task (IMU / mag  – not yet populated)
//   srv1      (pri 3) – Zenith  servo step/dir  (GPIO 0-3)
//   srv2      (pri 3) – Azimuth servo step/dir  (GPIO 4-7)
//   srv_ctrl  (pri 2) – reads location queues; posts ServoCmd to srv1/srv2
//   usb       (pri 1) – USB CDC logger + serial console
//
// All FreeRTOS objects are statically allocated — no heap usage for scheduler
// infrastructure.

#include "shared.hpp"

#include "Tasks/WiFi/wifi_task.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Tasks/Demo/demo_task.hpp"
// #include "Tasks/LoRa/lora_task.hpp"
// #include "Tasks/LoRa/lora2_task.hpp"
// #include "Tasks/GPS/gps_task.hpp"
// #include "Tasks/I2C/i2c_task.hpp"
// #include "Tasks/Servo/servo_task.hpp"
#include "Tasks/USB/usb_task.hpp"

#include "pico/stdlib.h"
#include <stdio.h>

// ── Shared FreeRTOS handles ───────────────────────────────────────────────────
EventGroupHandle_t g_net_events        = nullptr;
QueueHandle_t      g_mqtt_queue        = nullptr;
QueueHandle_t      g_log_queue         = nullptr;
// QueueHandle_t      g_gs_location_q     = nullptr;
// QueueHandle_t      g_rocket_location_q = nullptr;

// ── Static backing storage ────────────────────────────────────────────────────
static StaticEventGroup_t s_net_events_buf;

static StaticQueue_t s_mqtt_queue_buf;
static uint8_t       s_mqtt_queue_storage[ MQTT_QUEUE_DEPTH * sizeof(MqttMessage) ];

static StaticQueue_t s_log_queue_buf;
static uint8_t       s_log_queue_storage[ LOG_QUEUE_DEPTH  * sizeof(LogMessage)   ];

// static StaticQueue_t s_gs_location_buf;
// static uint8_t       s_gs_location_storage[ sizeof(LocationMsg) ];

// static StaticQueue_t s_rocket_location_buf;
// static uint8_t       s_rocket_location_storage[ sizeof(LocationMsg) ];

// ── FreeRTOS static-allocation callbacks ──────────────────────────────────────
extern "C" {

void vApplicationGetIdleTaskMemory( StaticTask_t** ppxIdleTaskTCBBuffer,
                                     StackType_t**  ppxIdleTaskStackBuffer,
                                     uint32_t*      pulIdleTaskStackSize )
{
    static StaticTask_t idle_tcb;
    static StackType_t  idle_stack[ configMINIMAL_STACK_SIZE ];
    *ppxIdleTaskTCBBuffer   = &idle_tcb;
    *ppxIdleTaskStackBuffer =  idle_stack;
    *pulIdleTaskStackSize   =  configMINIMAL_STACK_SIZE;
}

void vApplicationGetPassiveIdleTaskMemory( StaticTask_t** ppxIdleTaskTCBBuffer,
                                            StackType_t**  ppxIdleTaskStackBuffer,
                                            uint32_t*      pulIdleTaskStackSize,
                                            BaseType_t     xPassiveIdleTaskIndex )
{
    static StaticTask_t passive_tcb  [ configNUMBER_OF_CORES - 1 ];
    static StackType_t  passive_stack[ configNUMBER_OF_CORES - 1 ]
                                     [ configMINIMAL_STACK_SIZE ];
    *ppxIdleTaskTCBBuffer   = &passive_tcb  [ xPassiveIdleTaskIndex ];
    *ppxIdleTaskStackBuffer =  passive_stack[ xPassiveIdleTaskIndex ];
    *pulIdleTaskStackSize   =  configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t** ppxTimerTaskTCBBuffer,
                                      StackType_t**  ppxTimerTaskStackBuffer,
                                      uint32_t*      pulTimerTaskStackSize )
{
    static StaticTask_t timer_tcb;
    static StackType_t  timer_stack[ configTIMER_TASK_STACK_DEPTH ];
    *ppxTimerTaskTCBBuffer   = &timer_tcb;
    *ppxTimerTaskStackBuffer =  timer_stack;
    *pulTimerTaskStackSize   =  configTIMER_TASK_STACK_DEPTH;
}

} // extern "C"

// ── Entry point ───────────────────────────────────────────────────────────────
int main()
{
    stdio_init_all();

    // Give the USB CDC host a moment to connect before tasks start printing.
    // Without this, early log messages are silently dropped when no host is listening.
    sleep_ms( 3000 );

    printf( "Ground Station starting...\n" );

    for ( int i = 0; i < 10; ++i ) {
        printf( "." );
        sleep_ms( 500 );
    }
    printf( "\n" );

    // Shared synchronisation objects
    g_net_events = xEventGroupCreateStatic( &s_net_events_buf );

    g_mqtt_queue = xQueueCreateStatic( MQTT_QUEUE_DEPTH, sizeof(MqttMessage),
                                        s_mqtt_queue_storage, &s_mqtt_queue_buf );
    g_log_queue  = xQueueCreateStatic( LOG_QUEUE_DEPTH,  sizeof(LogMessage),
                                        s_log_queue_storage,  &s_log_queue_buf );

    printf( "Initializing tasks...\n" );

    // // Depth-1 overwrite queues — always carry the latest known position
    // g_gs_location_q     = xQueueCreateStatic( 1, sizeof(LocationMsg),
    //                                            s_gs_location_storage,
    //                                            &s_gs_location_buf );
    // g_rocket_location_q = xQueueCreateStatic( 1, sizeof(LocationMsg),
    //                                            s_rocket_location_storage,
    //                                            &s_rocket_location_buf );

    // ── Spawn tasks ───────────────────────────────────────────────────────────
    wifi_task_init();
    mqtt_task_init();
    demo_task_init();

    // lora1_task_init();
    // lora2_task_init();

    // gps_task_init();

    // i2c0_task_init();
    // i2c1_task_init();

    // servo1_task_init();
    // servo2_task_init();
    // servo_ctrl_task_init();

    printf( "Initializing USB task...\n" );

    usb_task_init();

    log_print( "System initialized.\n" );

    // Trigger heap initialization so xPortGetFreeHeapSize() reports real numbers
    { void *p = pvPortMalloc(1); vPortFree(p); }

    log_print( "Starting scheduler...\n" );

    vTaskStartScheduler();
    for ( ;; ) {}
}
