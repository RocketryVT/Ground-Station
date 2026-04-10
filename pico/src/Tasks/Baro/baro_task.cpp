#include "baro_task.hpp"
#include "shared.hpp"
#include "Tasks/I2C/i2c_task.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>
#include <string.h>

// -----------------------------------------------------------------------------
// MS5611  (I2C0 @ 0x77)
// -----------------------------------------------------------------------------

#define MS5611_ADDR             0x77    // CSB pin -> GND

#define MS5611_CMD_RESET        0x1E
#define MS5611_CMD_CONVERT_D1   0x48    // pressure,    OSR=4096 (~9.04 ms)
#define MS5611_CMD_CONVERT_D2   0x58    // temperature, OSR=4096 (~9.04 ms)
#define MS5611_CMD_ADC_READ     0x00

// Loop at 10 ms — 1 ms margin over the 9.04 ms OSR-4096 conversion time.
// Each cycle reads the previous result and triggers the next, so new pressure/
// temperature arrive every two cycles -> BaroMsg updates at ~50 Hz.
#define BARO_PERIOD_MS  10

// -----------------------------------------------------------------------------
// I2C reply queue (private to this task)
// -----------------------------------------------------------------------------

static StaticQueue_t s_reply_buf;
static uint8_t       s_reply_storage[ sizeof(I2cResponse) ];
static QueueHandle_t s_reply_q;

static bool i2c_cmd( uint8_t dev, uint8_t cmd )
{
    I2cRequest req = {};
    req.reply_q   = s_reply_q;
    req.addr      = dev;
    req.tx_buf[0] = cmd;
    req.tx_len    = 1;
    req.rx_len    = 0;
    if ( xQueueSend( g_i2c0_req_q, &req, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    I2cResponse resp;
    if ( xQueueReceive( s_reply_q, &resp, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    return ( resp.status == 0 );
}

static bool i2c_cmd_read( uint8_t dev, uint8_t cmd, uint8_t* out, uint8_t len )
{
    I2cRequest req = {};
    req.reply_q   = s_reply_q;
    req.addr      = dev;
    req.tx_buf[0] = cmd;
    req.tx_len    = 1;
    req.rx_len    = len;
    if ( xQueueSend( g_i2c0_req_q, &req, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    I2cResponse resp;
    if ( xQueueReceive( s_reply_q, &resp, pdMS_TO_TICKS(50) ) != pdTRUE ) return false;
    if ( resp.status != 0 ) return false;
    memcpy( out, resp.rx_buf, len );
    return true;
}

// -----------------------------------------------------------------------------
// MS5611 init and calculation
// -----------------------------------------------------------------------------

static uint16_t s_prom[8];  // C[0..7]; factory calibration, use C[1..6]

static bool ms5611_init()
{
    if ( !i2c_cmd( MS5611_ADDR, MS5611_CMD_RESET ) ) {
        log_print( "[baro] reset failed\n" );
        return false;
    }
    vTaskDelay( pdMS_TO_TICKS(5) );

    for ( int i = 0; i < 8; i++ ) {
        uint8_t buf[2];
        if ( !i2c_cmd_read( MS5611_ADDR, (uint8_t)( 0xA0 + i * 2 ), buf, 2 ) ) {
            log_print( "[baro] PROM read %d failed\n", i );
            return false;
        }
        s_prom[i] = (uint16_t)( ( buf[0] << 8 ) | buf[1] );
    }

    log_print( "[baro] MS5611 OK  C1=%u C2=%u C3=%u C4=%u C5=%u C6=%u\n",
               s_prom[1], s_prom[2], s_prom[3], s_prom[4], s_prom[5], s_prom[6] );

    // Trigger first D2 (temperature) so the first read has something waiting
    i2c_cmd( MS5611_ADDR, MS5611_CMD_CONVERT_D2 );
    return true;
}

// Full 2nd-order temperature compensation per MS5611 datasheet AN520.
static void ms5611_calculate( uint32_t D1, uint32_t D2,
                               float* pressure_pa, float* temp_c )
{
    const int32_t C1 = s_prom[1];
    const int32_t C2 = s_prom[2];
    const int32_t C3 = s_prom[3];
    const int32_t C4 = s_prom[4];
    const int32_t C5 = s_prom[5];
    const int32_t C6 = s_prom[6];

    int32_t dT   = (int32_t)D2 - C5 * 256;
    int32_t TEMP = 2000 + (int32_t)( (int64_t)dT * C6 / 8388608L );

    int64_t OFF  = (int64_t)C2 * 65536L  + (int64_t)C4 * dT / 128L;
    int64_t SENS = (int64_t)C1 * 32768L  + (int64_t)C3 * dT / 256L;

    int32_t T2   = 0;
    int64_t OFF2 = 0, SENS2 = 0;
    if ( TEMP < 2000 ) {
        int32_t dt2 = TEMP - 2000;
        T2    = (int32_t)( (int64_t)dT * dT / 2147483648LL );
        OFF2  = 5LL * dt2 * dt2 / 2;
        SENS2 = 5LL * dt2 * dt2 / 4;
        if ( TEMP < -1500 ) {
            int32_t dt3 = TEMP + 1500;
            OFF2  += 7LL  * dt3 * dt3;
            SENS2 += 11LL * dt3 * dt3 / 2;
        }
    }
    TEMP -= T2;
    OFF  -= OFF2;
    SENS -= SENS2;

    int32_t P = (int32_t)( ( (int64_t)D1 * SENS / 2097152L - OFF ) / 32768L );

    *temp_c      = TEMP / 100.0f;
    *pressure_pa = P / 100.0f;
}

// ISA altitude in metres MSL.  Pass actual QNH if known, else 101325 Pa.
static float baro_altitude( float pressure_pa, float qnh_pa )
{
    return 44330.0f * ( 1.0f - powf( pressure_pa / qnh_pa, 0.1902949f ) );
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

static StackType_t  s_stack[ 768 ];
static StaticTask_t s_tcb;

static void baro_task( void* )
{
    s_reply_q = xQueueCreateStatic( 1, sizeof(I2cResponse),
                                     s_reply_storage, &s_reply_buf );

    vTaskDelay( pdMS_TO_TICKS(200) );

    while ( !ms5611_init() ) {
        log_print( "[baro] init retry in 2 s\n" );
        vTaskDelay( pdMS_TO_TICKS(2000) );
    }

    // State machine: track which conversion result is waiting
    uint32_t D1 = 0, D2 = 0;
    bool     d2_pending = true;     // ms5611_init() triggered D2 first
    BaroMsg  msg        = {};

    TickType_t last_tick = xTaskGetTickCount();

    for ( ;; ) {
        // Read the ADC result that was triggered last cycle
        uint8_t adc_buf[3];
        if ( i2c_cmd_read( MS5611_ADDR, MS5611_CMD_ADC_READ, adc_buf, 3 ) ) {
            uint32_t adc = ( (uint32_t)adc_buf[0] << 16 )
                         | ( (uint32_t)adc_buf[1] <<  8 )
                         |             adc_buf[2];

            if ( d2_pending ) {
                D2 = adc;
                // Trigger D1 (pressure) for next cycle
                i2c_cmd( MS5611_ADDR, MS5611_CMD_CONVERT_D1 );
                d2_pending = false;
            } else {
                D1 = adc;
                // Trigger D2 (temperature) for next cycle
                i2c_cmd( MS5611_ADDR, MS5611_CMD_CONVERT_D2 );
                d2_pending = true;

                // Both D1 and D2 fresh — compute and publish
                if ( D1 > 0 && D2 > 0 ) {
                    float pres_pa, temp_c;
                    ms5611_calculate( D1, D2, &pres_pa, &temp_c );

                    msg.pressure_pa  = pres_pa;
                    msg.alt_m        = baro_altitude( pres_pa, 101325.0f );
                    msg.temp_c       = temp_c;
                    msg.timestamp_us = time_us_64();
                    msg.valid        = true;

                    xQueueOverwrite( g_baro_q, &msg );
                }
            }
        } else {
            // ADC read failed — re-trigger whichever conversion is pending
            i2c_cmd( MS5611_ADDR, d2_pending ? MS5611_CMD_CONVERT_D2
                                              : MS5611_CMD_CONVERT_D1 );
            log_print( "[baro] ADC read fail\n" );
        }

        vTaskDelayUntil( &last_tick, pdMS_TO_TICKS(BARO_PERIOD_MS) );
    }
}

void baro_task_init()
{
    task_create( baro_task, "baro", 768, nullptr, tskIDLE_PRIORITY + 3,
                 s_stack, &s_tcb );
}
