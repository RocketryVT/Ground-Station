#include "yaw_imu_task.hpp"
#include "shared.hpp"
#include "Tasks/I2C/i2c_task.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"
#include "Proto/mqtt_proto.hpp"

#include "nine_axis_imu/LSM6DSOX_LIS3MDL/LSM6DSOX_LIS3MDL.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>

#define YAW_IMU_SAMPLE_RATE_HZ  100
#define YAW_IMU_PERIOD_MS       ( 1000 / YAW_IMU_SAMPLE_RATE_HZ )
#define YAW_IMU_MQTT_INTERVAL_MS 50

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
    return resp.status == 0;
}

static bool i2c_read_regs( void* context, uint8_t dev, uint8_t reg, uint8_t* out, size_t len )
{
    if ( len > I2C_MAX_XFER ) return false;

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
    if ( resp.status != 0 || resp.rx_len < len ) return false;

    memcpy( out, resp.rx_buf, len );
    return true;
}

} // namespace

using Device = nine_axis_imu::lsm6dsox_lis3mdl::Device;

static bool yaw_imu_init( Device& device, uint8_t imu_addr )
{
    uint8_t imu_who = 0;
    const bool imu_who_ok = device.imu().read_who_am_i( imu_who );
    if ( !imu_who_ok ||
         ( imu_who != lsm6dsox::Device::kWhoAmIExpected &&
           imu_who != lsm6dsox::Device::kWhoAmICompatible ) ) {
        log_print( "[yaw_imu] no LSM6DSOX at 0x%02X (who_ok=%u who=0x%02X)\n",
                   imu_addr, imu_who_ok ? 1u : 0u, imu_who );
        return false;
    }

    if ( !device.imu().initialize() ) {
        log_print( "[yaw_imu] LSM6DSOX init failed at 0x%02X (WHO=0x%02X)\n",
                   imu_addr, imu_who );
        return false;
    }

    uint8_t mag_who = 0;
    const bool mag_who_ok = device.mag().read_who_am_i( mag_who );
    if ( !mag_who_ok || mag_who != lis3mdl::Device::kWhoAmIExpected ) {
        log_print( "[yaw_imu] LIS3MDL not found at 0x%02X (who_ok=%u who=0x%02X)\n",
                   lis3mdl::Device::kDefaultAddress,
                   mag_who_ok ? 1u : 0u,
                   mag_who );
        return false;
    }

    if ( !device.mag().initialize() ) {
        log_print( "[yaw_imu] LIS3MDL init failed at 0x%02X (WHO=0x%02X)\n",
                   lis3mdl::Device::kDefaultAddress, mag_who );
        return false;
    }

    log_print( "[yaw_imu] LSM6DSOX+LIS3MDL OK on I2C1 imu=0x%02X mag=0x%02X\n",
               imu_addr, lis3mdl::Device::kDefaultAddress );
    return true;
}

static bool yaw_imu_read( Device const& device, YawImuMsg& m )
{
    nine_axis_imu::lsm6dsox_lis3mdl::Sample sample = {};
    if ( !device.read_sample( sample ) ) {
        return false;
    }

    m.accel[0] = sample.accel[0];
    m.accel[1] = sample.accel[1];
    m.accel[2] = sample.accel[2];
    m.gyro[0] = sample.gyro[0];
    m.gyro[1] = sample.gyro[1];
    m.gyro[2] = sample.gyro[2];
    // LIS3MDL reports in gauss; YawImuMsg expects µT (1 gauss = 100 µT)
    m.mag_ut[0] = sample.mag_gauss[0] * 100.0f;
    m.mag_ut[1] = sample.mag_gauss[1] * 100.0f;
    m.mag_ut[2] = sample.mag_gauss[2] * 100.0f;
    m.temp_c = sample.temp_c;
    m.mag_valid = sample.mag_valid;
    m.mag_overflow = false;
    return true;
}

static StackType_t  s_stack[ 768 ];
static StaticTask_t s_tcb;

static void yaw_imu_task( void* )
{
    s_reply_q = xQueueCreateStatic( 1, sizeof(I2cResponse),
                                     s_reply_storage, &s_reply_buf );

    s_bus_ctx.request_q = g_i2c1_req_q;
    s_bus_ctx.reply_q = s_reply_q;

    const nine_axis_imu::lsm6dsox_lis3mdl::Transport transport = {
        .ctx = &s_bus_ctx,
        .write_reg = i2c_write_reg,
        .read_regs = i2c_read_regs,
    };
    Device device_a( transport, lsm6dsox::Device::kDefaultAddress,
                     lis3mdl::Device::kDefaultAddress );
    Device device_b( transport, lsm6dsox::Device::kAltAddress,
                     lis3mdl::Device::kDefaultAddress );
    Device* device = nullptr;

    vTaskDelay( pdMS_TO_TICKS(200) );

    while ( true ) {
        if ( yaw_imu_init( device_a, lsm6dsox::Device::kDefaultAddress ) ) {
            device = &device_a;
            break;
        }
        if ( yaw_imu_init( device_b, lsm6dsox::Device::kAltAddress ) ) {
            device = &device_b;
            break;
        }
        log_print( "[yaw_imu] init retry in 2 s\n" );
        vTaskDelay( pdMS_TO_TICKS(2000) );
    }

    TickType_t last_tick = xTaskGetTickCount();
    TickType_t last_mqtt = xTaskGetTickCount();
    TickType_t last_read_fail_log = 0;

    for ( ;; ) {
        YawImuMsg m = {};
        m.timestamp_us = time_us_64();

        if ( yaw_imu_read( *device, m ) ) {
            m.valid = true;
            xQueueOverwrite( g_yaw_imu_q, &m );

            TickType_t now_ticks = xTaskGetTickCount();
            if ( mqtt_raw_yaw_imu_enabled()
                 && mqtt_is_connected()
                 && ( now_ticks - last_mqtt ) >= pdMS_TO_TICKS(YAW_IMU_MQTT_INTERVAL_MS) )
            {
                last_mqtt = now_ticks;

                MqttMessage msg = {};
                groundstation_RawYawImuSample pb =
                    groundstation_RawYawImuSample_init_zero;
                pb.has_timestamp = true; pb.timestamp = m.timestamp_us / 1000u;
                pb.has_ax = true; pb.ax = m.accel[0];
                pb.has_ay = true; pb.ay = m.accel[1];
                pb.has_az = true; pb.az = m.accel[2];
                pb.has_gx = true; pb.gx = m.gyro[0];
                pb.has_gy = true; pb.gy = m.gyro[1];
                pb.has_gz = true; pb.gz = m.gyro[2];
                pb.has_mx_ut = true; pb.mx_ut = m.mag_ut[0];
                pb.has_my_ut = true; pb.my_ut = m.mag_ut[1];
                pb.has_mz_ut = true; pb.mz_ut = m.mag_ut[2];
                pb.has_mag_valid = true; pb.mag_valid = m.mag_valid;
                pb.has_mag_overflow = true; pb.mag_overflow = m.mag_overflow;
                pb.has_temp = true; pb.temp = m.temp_c;

                if ( mqtt_encode_proto( msg, "gs/pico/primary/raw/yaw_imu",
                                        groundstation_RawYawImuSample_fields, &pb ) )
                    xQueueSend( g_mqtt_queue, &msg, 0 );
            }
        } else if ( xTaskGetTickCount() - last_read_fail_log >= pdMS_TO_TICKS(1000) ) {
            last_read_fail_log = xTaskGetTickCount();
            log_print( "[yaw_imu] read fail\n" );
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(YAW_IMU_PERIOD_MS) );
    }
}

void yaw_imu_task_init()
{
    task_create( yaw_imu_task, "yaw_imu", 768, nullptr, tskIDLE_PRIORITY + 3,
                 s_stack, &s_tcb );
}
