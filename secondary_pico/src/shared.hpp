#pragma once

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "pico/platform/panic.h"
#include <stdio.h>

// -- Task creation helper -------------------------------------------------------
static inline TaskHandle_t task_create( TaskFunction_t fn,
                                         const char*    name,
                                         uint32_t       stack_depth,
                                         void*          param,
                                         UBaseType_t    priority,
                                         StackType_t*   stack,
                                         StaticTask_t*  tcb )
{
    TaskHandle_t h = xTaskCreateStatic( fn, name, stack_depth, param,
                                         priority, stack, tcb );
    if ( !h )
        panic( "[rtos] xTaskCreateStatic failed: '%s' stack=%u pri=%u",
               name, (unsigned)stack_depth, (unsigned)priority );
    return h;
}

// -- Log queue -----------------------------------------------------------------
struct LogMessage {
    char buf[ 256 ];
};

#define LOG_QUEUE_DEPTH  32
extern QueueHandle_t g_log_queue;

void log_print( const char* fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));

// -- Network event bits --------------------------------------------------------
#define EVT_WIFI_CONNECTED   ( 1 << 0 )

extern EventGroupHandle_t g_net_events;

// -- UDP forwarding queue ------------------------------------------------------
// LoRa tasks enqueue raw InterPico frames (framed SIGMA bytes) here.
// The UDP task drains it and sends each frame to the primary Pico.
//
// Max SIGMA frame = payload(60) + overhead(10) = 70 bytes for INTER_PICO.
#define INTER_PICO_FRAME_MAX  96
#define UDP_QUEUE_DEPTH       8

struct UdpFrame {
    uint8_t  data[ INTER_PICO_FRAME_MAX ];
    uint16_t len;
};

extern QueueHandle_t g_udp_queue;

// -- WiFi / network configuration ----------------------------------------------
#define WIFI_SSID         "ssid"       // change to match your Starlink SSID
#define WIFI_PASSWORD     "password"   // change to match your Starlink password

// Primary Pico IP and UDP port for InterPico forwarding.
// Assign a static IP to the primary Pico via Starlink's DHCP reservation,
// or use a link-local address if both Picos are on the same AP.
#define PRIMARY_PICO_IP   "192.168.100.2"   // set to primary Pico's static IP
#define INTER_PICO_PORT   5005

// -- Pin assignments -----------------------------------------------------------
// Single source of truth for the Ground Station PCB rev 1 (Pico 2 W). This
// secondary Pico runs on the same GSPCB layout as the primary:
//   915 MHz LoRa (SX1276, SPI0) -> Pins::LORA0_*   (this project's lora1_task)
//   433 MHz GFSK (RFM69, SPI1)  -> Pins::LORA1_*   (this project's lora2_task)
#include "boards/gs_pcb_v1_pins.hpp"

// -- LoRa radio parameters (must match rocket transmitter settings) -------------
namespace LoRa1Cfg {
    static constexpr float    FREQ_MHZ   = 915.0f;
    static constexpr float    BW_KHZ     = 125.0f;
    static constexpr uint8_t  SF         = 7;
    static constexpr uint8_t  CR         = 5;
    static constexpr uint8_t  SYNC_WORD  = 0x12;
    static constexpr int8_t   TX_POWER   = 20;
    static constexpr uint16_t PREAMBLE   = 8;
}

namespace LoRa2Cfg {
    static constexpr float   FREQ_MHZ    = 424.5f;
    static constexpr float   BR_KBPS     = 4.8f;
    static constexpr float   FREQ_DEV_KHZ = 5.0f;
    static constexpr float   RX_BW_KHZ   = 125.0f;
    static constexpr int8_t  TX_POWER    = 20;
    static constexpr uint8_t PREAMBLE    = 16;
}
