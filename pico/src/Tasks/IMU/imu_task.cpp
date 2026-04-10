#include "imu_task.hpp"
#include "shared.hpp"
#include "Tasks/I2C/i2c_task.hpp"

#include "ism330dlc/ISM330DLC.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <string.h>

#define IMU_SAMPLE_RATE_HZ  100
#define IMU_PERIOD_MS       ( 1000 / IMU_SAMPLE_RATE_HZ )

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

static bool imu_init( ism330dlc::Device& device )
{
    uint8_t who = 0;
    if ( !device.read_who_am_i( who ) ) {
        log_print( "[imu] WHO_AM_I read failed\n" );
        return false;
    }
    if ( who != ism330dlc::Device::kWhoAmIExpected ) {
        log_print( "[imu] WHO_AM_I mismatch: got 0x%02X expected 0x%02X\n",
                   who, ism330dlc::Device::kWhoAmIExpected );
        return false;
    }

    // 104 Hz, 16g / 2000 dps — same as before but now explicit and runtime-changeable
    const ism330dlc::Config cfg = {
        .accel_range = ism330dlc::AccelRange::g16,
        .gyro_range  = ism330dlc::GyroRange::dps_2000,
        .accel_odr   = ism330dlc::AccelODR::hz_104,
        .gyro_odr    = ism330dlc::GyroODR::hz_104,
        .delay_ms    = []( uint32_t ms ){ vTaskDelay( pdMS_TO_TICKS(ms) ); },
    };

    if ( !device.initialize( cfg ) ) {
        log_print( "[imu] ISM330DLC init failed\n" );
        return false;
    }

    log_print( "[imu] ISM330DLC OK (WHO_AM_I=0x%02X)\n", who );
    return true;
}

static bool imu_read( const ism330dlc::Device& device, IcmMsg& m )
{
    ism330dlc::Sample sample = {};
    if ( !device.read_sample( sample ) ) {
        return false;
    }

    m.accel[0] = sample.accel_x;
    m.accel[1] = sample.accel_y;
    m.accel[2] = sample.accel_z;
    m.gyro[0]  = sample.gyro_x;
    m.gyro[1]  = sample.gyro_y;
    m.gyro[2]  = sample.gyro_z;
    m.temp_c   = sample.temp_c;
    return true;
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

static StackType_t  s_stack[ 512 ];
static StaticTask_t s_tcb;

static void imu_task( void* )
{
    s_reply_q = xQueueCreateStatic( 1, sizeof(I2cResponse),
                                     s_reply_storage, &s_reply_buf );

    s_bus_ctx.request_q = g_i2c0_req_q;
    s_bus_ctx.reply_q   = s_reply_q;

    const ism330dlc::Transport transport = {
        .ctx        = &s_bus_ctx,
        .write_reg  = i2c_write_reg,
        .read_regs  = i2c_read_regs,
    };
    ism330dlc::Device device( transport );

    vTaskDelay( pdMS_TO_TICKS(200) );   // wait for I2C tasks to start

    while ( !imu_init( device ) ) {
        log_print( "[imu] init retry in 2 s\n" );
        vTaskDelay( pdMS_TO_TICKS(2000) );
    }

    TickType_t last_tick = xTaskGetTickCount();

    for ( ;; ) {
        IcmMsg m = {};
        m.timestamp_us = time_us_64();

        if ( imu_read( device, m ) ) {
            m.valid = true;
            xQueueOverwrite( g_icm_q, &m );
        } else {
            log_print( "[imu] read fail\n" );
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(IMU_PERIOD_MS) );
    }
}

void imu_task_init()
{
    task_create( imu_task, "imu", 512, nullptr, tskIDLE_PRIORITY + 4,
                 s_stack, &s_tcb );
}
