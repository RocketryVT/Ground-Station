#include "i2c_task.hpp"
#include "shared.hpp"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

// ── Queue handles (exported via i2c_task.hpp) ─────────────────────────────────
QueueHandle_t g_i2c0_req_q = nullptr;
QueueHandle_t g_i2c1_req_q = nullptr;

static StaticQueue_t s_i2c0_req_buf;
static uint8_t       s_i2c0_req_storage[ I2C_REQ_QUEUE_DEPTH * sizeof(I2cRequest) ];

static StaticQueue_t s_i2c1_req_buf;
static uint8_t       s_i2c1_req_storage[ I2C_REQ_QUEUE_DEPTH * sizeof(I2cRequest) ];

// ── Bus configuration ─────────────────────────────────────────────────────────
struct I2cBusCfg {
    i2c_inst_t*    bus;
    uint           sda;
    uint           scl;
    uint32_t       baud_hz;
    QueueHandle_t* req_q_ptr;  // pointer to the queue handle (set before task starts)
    const char*    tag;
};

// ── Generic bus worker ────────────────────────────────────────────────────────
// A single task function serves both I2C0 and I2C1; the per-bus config is
// passed via the FreeRTOS task parameter.
static void i2c_bus_task( void* arg )
{
    const I2cBusCfg* cfg = static_cast<const I2cBusCfg*>( arg );

    i2c_init( cfg->bus, cfg->baud_hz );
    gpio_set_function( cfg->sda, GPIO_FUNC_I2C );
    gpio_set_function( cfg->scl, GPIO_FUNC_I2C );
    gpio_pull_up( cfg->sda );
    gpio_pull_up( cfg->scl );

    log_print( "[%s] ready at %lu Hz  SDA=GPIO%u  SCL=GPIO%u\n",
               cfg->tag, (unsigned long)cfg->baud_hz, cfg->sda, cfg->scl );

    QueueHandle_t req_q = *cfg->req_q_ptr;

    for ( ;; ) {
        I2cRequest req;
        xQueueReceive( req_q, &req, portMAX_DELAY );

        I2cResponse resp = {};

        // ── Write phase ───────────────────────────────────────────────────────
        if ( req.tx_len > 0 ) {
            // nostop=true when a read follows so the bus isn't released between
            bool nostop = ( req.rx_len > 0 );
            int rc = i2c_write_blocking( cfg->bus, req.addr,
                                         req.tx_buf, req.tx_len, nostop );
            if ( rc < 0 ) {
                resp.status = rc;
                xQueueSend( req.reply_q, &resp, 0 );
                continue;
            }
        }

        // ── Read phase ────────────────────────────────────────────────────────
        if ( req.rx_len > 0 ) {
            int rc = i2c_read_blocking( cfg->bus, req.addr,
                                        resp.rx_buf, req.rx_len, false );
            if ( rc < 0 ) {
                resp.status = rc;
            } else {
                resp.status = 0;
                resp.rx_len = static_cast<uint8_t>( rc );
            }
        } else {
            resp.status = 0;
        }

        xQueueSend( req.reply_q, &resp, 0 );
    }
}

// ── I2C0 ──────────────────────────────────────────────────────────────────────
static I2cBusCfg s_i2c0_cfg = {
    .bus        = i2c0,
    .sda        = Pins::I2C0_SDA,
    .scl        = Pins::I2C0_SCL,
    .baud_hz    = 400'000u,
    .req_q_ptr  = &g_i2c0_req_q,
    .tag        = "i2c0",
};

static StaticTask_t s_i2c0_tcb;
static StackType_t  s_i2c0_stack[ 512 ];

void i2c0_task_init()
{
    g_i2c0_req_q = xQueueCreateStatic( I2C_REQ_QUEUE_DEPTH, sizeof(I2cRequest),
                                        s_i2c0_req_storage, &s_i2c0_req_buf );
    configASSERT( g_i2c0_req_q );

    task_create( i2c_bus_task, "i2c0", 512, &s_i2c0_cfg, tskIDLE_PRIORITY + 2,
                  s_i2c0_stack, &s_i2c0_tcb );
}

// ── I2C1 ──────────────────────────────────────────────────────────────────────
static I2cBusCfg s_i2c1_cfg = {
    .bus        = i2c1,
    .sda        = Pins::I2C1_SDA,
    .scl        = Pins::I2C1_SCL,
    .baud_hz    = 400'000u,
    .req_q_ptr  = &g_i2c1_req_q,
    .tag        = "i2c1",
};

static StaticTask_t s_i2c1_tcb;
static StackType_t  s_i2c1_stack[ 512 ];

void i2c1_task_init()
{
    g_i2c1_req_q = xQueueCreateStatic( I2C_REQ_QUEUE_DEPTH, sizeof(I2cRequest),
                                        s_i2c1_req_storage, &s_i2c1_req_buf );
    configASSERT( g_i2c1_req_q );

    task_create( i2c_bus_task, "i2c1", 512, &s_i2c1_cfg, tskIDLE_PRIORITY + 2,
                  s_i2c1_stack, &s_i2c1_tcb );
}
