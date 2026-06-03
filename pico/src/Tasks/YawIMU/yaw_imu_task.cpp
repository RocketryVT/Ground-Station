#include "yaw_imu_task.hpp"
#include "shared.hpp"
#include "Tasks/I2C/i2c_task.hpp"
#include "Tasks/MQTT/mqtt_task.hpp"

#include "nine_axis_imu/ICM20948.hpp"

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

static void delay_ms_task( uint32_t ms )
{
    vTaskDelay( pdMS_TO_TICKS(ms) );
}

} // namespace

static bool yaw_imu_init( nine_axis_imu::ICM20948& device )
{
    uint8_t who = 0;
    if ( !device.read_who_am_i( who ) ) {
        log_print( "[yaw_imu] WHO_AM_I read failed\n" );
        return false;
    }
    if ( who != nine_axis_imu::ICM20948::kWhoAmIExpected ) {
        log_print( "[yaw_imu] WHO_AM_I mismatch: got 0x%02X expected 0x%02X\n",
                   who, nine_axis_imu::ICM20948::kWhoAmIExpected );
        return false;
    }

    const nine_axis_imu::Config cfg = {
        .accel_range = nine_axis_imu::AccelRange::g16,
        .gyro_range = nine_axis_imu::GyroRange::dps_2000,
        .accel_rate_hz = YAW_IMU_SAMPLE_RATE_HZ,
        .gyro_rate_hz = YAW_IMU_SAMPLE_RATE_HZ,
        .accel_dlpf_enable = true,
        .gyro_dlpf_enable = true,
        .accel_dlpf = nine_axis_imu::DlpfBandwidth::hz_23_9,
        .gyro_dlpf = nine_axis_imu::DlpfBandwidth::hz_23_9,
        .mag_mode = nine_axis_imu::MagMode::hz_100,
        .delay_ms = delay_ms_task,
    };

    if ( !device.initialize( cfg ) ) {
        log_print( "[yaw_imu] ICM-20948 init failed\n" );
        return false;
    }

    uint16_t mag_id = 0;
    device.read_mag_who_am_i( mag_id );
    log_print( "[yaw_imu] ICM-20948 OK on I2C1 (WHO_AM_I=0x%02X AK09916=0x%04X)\n",
               who, mag_id );
    return true;
}

static bool yaw_imu_read( const nine_axis_imu::ICM20948& device, YawImuMsg& m )
{
    nine_axis_imu::Sample sample = {};
    if ( !device.read_sample( sample ) ) {
        return false;
    }

    m.accel[0] = sample.accel_x;
    m.accel[1] = sample.accel_y;
    m.accel[2] = sample.accel_z;
    m.gyro[0] = sample.gyro_x;
    m.gyro[1] = sample.gyro_y;
    m.gyro[2] = sample.gyro_z;
    m.mag_ut[0] = sample.mag_x_ut;
    m.mag_ut[1] = sample.mag_y_ut;
    m.mag_ut[2] = sample.mag_z_ut;
    m.temp_c = sample.temp_c;
    m.mag_valid = sample.mag_valid;
    m.mag_overflow = sample.mag_overflow;
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

    const nine_axis_imu::Transport transport = {
        .ctx = &s_bus_ctx,
        .write_reg = i2c_write_reg,
        .read_regs = i2c_read_regs,
    };
    nine_axis_imu::ICM20948 default_addr_device( transport, nine_axis_imu::ICM20948::kDefaultAddress );
    nine_axis_imu::ICM20948 alt_addr_device( transport, nine_axis_imu::ICM20948::kAltAddress );
    nine_axis_imu::ICM20948* device = &default_addr_device;

    vTaskDelay( pdMS_TO_TICKS(200) );

    while ( true ) {
        if ( yaw_imu_init( default_addr_device ) ) {
            device = &default_addr_device;
            break;
        }
        if ( yaw_imu_init( alt_addr_device ) ) {
            device = &alt_addr_device;
            break;
        }
        log_print( "[yaw_imu] init retry in 2 s\n" );
        vTaskDelay( pdMS_TO_TICKS(2000) );
    }

    TickType_t last_tick = xTaskGetTickCount();
    TickType_t last_mqtt = xTaskGetTickCount();

    for ( ;; ) {
        YawImuMsg m = {};
        m.timestamp_us = time_us_64();

        if ( yaw_imu_read( *device, m ) ) {
            m.valid = true;
            xQueueOverwrite( g_yaw_imu_q, &m );

            TickType_t now_ticks = xTaskGetTickCount();
            if ( mqtt_raw_imu_enabled()
                 && mqtt_is_connected()
                 && ( now_ticks - last_mqtt ) >= pdMS_TO_TICKS(YAW_IMU_MQTT_INTERVAL_MS) )
            {
                last_mqtt = now_ticks;

                MqttMessage msg = {};
                snprintf( msg.topic, sizeof(msg.topic), "gs/pico/primary/raw/yaw_imu" );
                snprintf( msg.payload, sizeof(msg.payload),
                          "{"
                          "\"timestamp\":%llu,"
                          "\"ax\":%.5f,\"ay\":%.5f,\"az\":%.5f,"
                          "\"gx\":%.5f,\"gy\":%.5f,\"gz\":%.5f,"
                          "\"mx_ut\":%.3f,\"my_ut\":%.3f,\"mz_ut\":%.3f,"
                          "\"mag_valid\":%s,\"mag_overflow\":%s,"
                          "\"temp\":%.2f"
                          "}",
                          (unsigned long long)( m.timestamp_us / 1000u ),
                          (double)m.accel[0], (double)m.accel[1], (double)m.accel[2],
                          (double)m.gyro[0], (double)m.gyro[1], (double)m.gyro[2],
                          (double)m.mag_ut[0], (double)m.mag_ut[1], (double)m.mag_ut[2],
                          m.mag_valid ? "true" : "false",
                          m.mag_overflow ? "true" : "false",
                          (double)m.temp_c );
                xQueueSend( g_mqtt_queue, &msg, 0 );
            }
        } else {
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
