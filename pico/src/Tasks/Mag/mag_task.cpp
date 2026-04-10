#include "mag_task.hpp"
#include "shared.hpp"
#include "Tasks/I2C/i2c_task.hpp"

#include "lis3mdl/LIS3MDL.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <string.h>

#define MAG_SAMPLE_RATE_HZ  80
#define MAG_PERIOD_MS       ( 1000 / MAG_SAMPLE_RATE_HZ )  // 12 ms (rounds down, ~83 Hz)

// -----------------------------------------------------------------------------
// I2C reply queue (private to this task)
// -----------------------------------------------------------------------------

static StaticQueue_t s_reply_buf;
static uint8_t       s_reply_storage[ sizeof(I2cResponse) ];
static QueueHandle_t s_reply_q;

namespace {

struct I2cTaskBusContext {
    QueueHandle_t request_q;
    QueueHandle_t reply_q;
};

static I2cTaskBusContext s_bus_ctx = {};

static bool i2c_write_reg( void* context, uint8_t dev, uint8_t reg, uint8_t val )
{
    auto* bus_ctx = static_cast<I2cTaskBusContext*>( context );

    I2cRequest req = {};
    req.reply_q   = bus_ctx->reply_q;
    req.addr      = dev;
    req.tx_buf[0] = reg;
    req.tx_buf[1] = val;
    req.tx_len    = 2;
    req.rx_len    = 0;
    if ( xQueueSend( bus_ctx->request_q, &req, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;

    I2cResponse resp = {};
    if ( xQueueReceive( bus_ctx->reply_q, &resp, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    return ( resp.status == 0 );
}

static bool i2c_read_regs( void* context, uint8_t dev, uint8_t reg, uint8_t* out, size_t len )
{
    auto* bus_ctx = static_cast<I2cTaskBusContext*>( context );

    I2cRequest req = {};
    req.reply_q   = bus_ctx->reply_q;
    req.addr      = dev;
    req.tx_buf[0] = reg;
    req.tx_len    = 1;
    req.rx_len    = static_cast<uint8_t>( len );
    if ( xQueueSend( bus_ctx->request_q, &req, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;

    I2cResponse resp = {};
    if ( xQueueReceive( bus_ctx->reply_q, &resp, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    if ( resp.status != 0 ) return false;

    memcpy( out, resp.rx_buf, len );
    return true;
}

} // namespace

static void delay_ms_task( uint32_t ms ) { vTaskDelay( pdMS_TO_TICKS(ms) ); }

static bool lis_init( lis3mdl::Device& device )
{
    // UHP @ 80 Hz: maximum internal oversampling (lowest noise floor) and
    // narrowest output bandwidth — best rejection of stepper motor interference.
    const lis3mdl::Config cfg = {
        .range     = lis3mdl::Range::gauss_4,
        .data_rate = lis3mdl::DataRate::hz_80,
        .perf_mode = lis3mdl::PerformanceMode::ultra_high,
        .op_mode   = lis3mdl::OperationMode::continuous,
        .delay_ms  = delay_ms_task,
    };
    if ( !device.initialize( cfg ) ) {
        log_print( "[mag] LIS3MDL init failed\n" );
        return false;
    }
    log_print( "[mag] LIS3MDL OK\n" );
    return true;
}

static bool lis_read( const lis3mdl::Device& device, MagMsg& m )
{
    lis3mdl::Sample sample = {};
    if ( !device.read_sample( sample ) ) {
        return false;
    }

    m.mag[0] = sample.x_gauss;
    m.mag[1] = sample.y_gauss;
    m.mag[2] = sample.z_gauss;
    return true;
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

static StackType_t  s_stack[ 512 ];
static StaticTask_t s_tcb;

static void mag_task( void* )
{
    s_reply_q = xQueueCreateStatic( 1, sizeof(I2cResponse),
                                     s_reply_storage, &s_reply_buf );

    s_bus_ctx.request_q = g_i2c0_req_q;
    s_bus_ctx.reply_q   = s_reply_q;

    const lis3mdl::Transport transport = {
        .ctx        = &s_bus_ctx,
        .write_reg  = i2c_write_reg,
        .read_regs  = i2c_read_regs,
    };
    lis3mdl::Device device( transport );

    vTaskDelay( pdMS_TO_TICKS(200) );

    while ( !lis_init( device ) ) {
        log_print( "[mag] init retry in 2 s\n" );
        vTaskDelay( pdMS_TO_TICKS(2000) );
    }

    TickType_t last_tick = xTaskGetTickCount();

    for ( ;; ) {
        MagMsg m = {};
        m.timestamp_us = time_us_64();

        if ( lis_read( device, m ) ) {
            m.valid = true;
            xQueueOverwrite( g_mag_q, &m );
        } else {
            log_print( "[mag] read fail\n" );
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(MAG_PERIOD_MS) );
    }
}

void mag_task_init()
{
    task_create( mag_task, "mag", 512, nullptr, tskIDLE_PRIORITY + 3,
                 s_stack, &s_tcb );
}
