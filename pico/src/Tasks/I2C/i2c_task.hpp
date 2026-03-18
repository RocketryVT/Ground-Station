#pragma once

#include "FreeRTOS.h"
#include "queue.h"

// ── I2C Bus Tasks ─────────────────────────────────────────────────────────────
// Each I2C bus (I2C0, I2C1) runs in its own FreeRTOS task and serialises all
// bus traffic through a request / response queue pair.
//
// ── Usage from a client task ─────────────────────────────────────────────────
//
//   // 1. Create a per-task depth-1 static reply queue (once at task start):
//   static StaticQueue_t  rep_buf;
//   static uint8_t        rep_storage[ sizeof(I2cResponse) ];
//   QueueHandle_t my_reply_q = xQueueCreateStatic( 1, sizeof(I2cResponse),
//                                                   rep_storage, &rep_buf );
//
//   // 2. Build a request and send it to the bus task:
//   I2cRequest req = {};
//   req.reply_q      = my_reply_q;
//   req.addr         = 0x76;   // 7-bit device address
//   req.tx_buf[0]    = 0xF3;   // register / command byte
//   req.tx_len       = 1;
//   req.rx_len       = 2;      // bytes to read back (0 = write-only)
//   xQueueSend( g_i2c0_req_q, &req, pdMS_TO_TICKS(100) );
//
//   // 3. Wait for the response:
//   I2cResponse resp;
//   xQueueReceive( my_reply_q, &resp, pdMS_TO_TICKS(100) );
//   if ( resp.status == 0 ) { /* resp.rx_buf[0..rx_len-1] holds the data */ }
//
// Notes:
//   • I2cRequest / I2cResponse are copied by value through the queues — no
//     pointers into the caller's stack are held after xQueueSend returns.
//   • The bus task uses i2c_write_blocking / i2c_read_blocking; it will block
//     the I2C bus task for the duration but not the calling task.
//   • I2C0 and I2C1 are initialised inside the task, so devices should not be
//     accessed until after vTaskStartScheduler() has run.

#define I2C_MAX_XFER  32

struct I2cRequest {
    QueueHandle_t  reply_q;                  // where to deliver the response
    uint8_t        addr;                     // 7-bit device address
    uint8_t        tx_buf[ I2C_MAX_XFER ];   // bytes to write first
    uint8_t        tx_len;                   // number of bytes to write (0 = skip)
    uint8_t        rx_len;                   // bytes to read after write (0 = write-only)
};

struct I2cResponse {
    int     status;                          // 0 = OK, negative = Pico SDK error
    uint8_t rx_buf[ I2C_MAX_XFER ];         // received bytes (valid when status == 0)
    uint8_t rx_len;                          // actual bytes received
};

// ── Request queue handles ──────────────────────────────────────────────────────
// Send an I2cRequest to whichever bus holds your device.
// The response arrives on req.reply_q.
extern QueueHandle_t g_i2c0_req_q;
extern QueueHandle_t g_i2c1_req_q;

#define I2C_REQ_QUEUE_DEPTH  8

// ── Init functions ─────────────────────────────────────────────────────────────
// Call before vTaskStartScheduler().  Each function creates the request queue
// and spawns the bus task.
void i2c0_task_init();
void i2c1_task_init();
