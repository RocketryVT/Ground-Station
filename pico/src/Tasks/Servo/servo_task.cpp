#include "servo_task.hpp"
#include "shared.hpp"

#include "math_utils/GroundStationMath.h"

#include "hardware/gpio.h"
#include "pico/time.h"

// ── Command queue backing storage ─────────────────────────────────────────────
QueueHandle_t g_servo1_cmd_q = nullptr;
QueueHandle_t g_servo2_cmd_q = nullptr;

static StaticQueue_t s_srv1_cmd_buf;
static uint8_t       s_srv1_cmd_storage[ sizeof(ServoCmd) ];  // depth 1

static StaticQueue_t s_srv2_cmd_buf;
static uint8_t       s_srv2_cmd_storage[ sizeof(ServoCmd) ];  // depth 1

// ── Hardware task config ───────────────────────────────────────────────────────
struct ServoHwCfg {
    uint           pul_p;
    uint           pul_n;
    uint           dir_p;
    uint           dir_n;
    QueueHandle_t* cmd_q_ptr;
    const char*    tag;
};

// ── Step pulse helpers ────────────────────────────────────────────────────────
static inline void set_direction( const ServoHwCfg* hw, bool forward )
{
    gpio_put( hw->dir_p, forward ? 1u : 0u );
    gpio_put( hw->dir_n, forward ? 0u : 1u );
    sleep_us( ServoCfg::DIR_SETUP_US );
}

static inline void step_pulse( const ServoHwCfg* hw )
{
    gpio_put( hw->pul_p, 1u );
    gpio_put( hw->pul_n, 0u );
    sleep_us( ServoCfg::PULSE_US );
    gpio_put( hw->pul_p, 0u );
    gpio_put( hw->pul_n, 1u );
    sleep_us( ServoCfg::PULSE_US );   // low dwell = same width as high
}

// ── Generic step/dir hardware task ───────────────────────────────────────────
// One instance runs per servo axis.  The task owns its four GPIO pins and
// executes step pulses toward the target position held in the command queue.
//
// The command queue has depth 1.  The controller uses xQueueOverwrite() so
// this task always sees the latest target; xQueuePeek() is non-destructive
// so the last target persists even when no new command arrives.
static void servo_hw_task( void* arg )
{
    const ServoHwCfg* hw = static_cast<const ServoHwCfg*>( arg );

    // Initialise GPIO – idle state: PUL low-side asserted, DIR forward
    const uint pins[4] = { hw->pul_p, hw->pul_n, hw->dir_p, hw->dir_n };
    for ( uint p : pins ) {
        gpio_init( p );
        gpio_set_dir( p, GPIO_OUT );
    }
    gpio_put( hw->pul_p, 0u );  gpio_put( hw->pul_n, 1u );
    gpio_put( hw->dir_p, 0u );  gpio_put( hw->dir_n, 1u );

    log_print( "[%s] differential step/dir ready"
               "  PUL+=%u PUL-=%u  DIR+=%u DIR-=%u\n",
               hw->tag,
               hw->pul_p, hw->pul_n, hw->dir_p, hw->dir_n );

    QueueHandle_t cmd_q = *hw->cmd_q_ptr;

    int32_t  current_steps = 0;
    int32_t  target_steps  = 0;
    uint16_t step_hz       = ServoCfg::DEFAULT_STEP_HZ;

    // µs delay between steps → convert from hz; recomputed on each new command
    uint32_t step_period_us = 1'000'000u / step_hz;

    for ( ;; ) {
        // Non-destructive peek — picks up the latest overwritten target
        ServoCmd cmd;
        if ( xQueuePeek( cmd_q, &cmd, 0 ) == pdTRUE ) {
            target_steps = cmd.target_steps;
            if ( cmd.step_hz > 0 ) {
                step_hz        = cmd.step_hz;
                step_period_us = 1'000'000u / step_hz;
            }
        }

        if ( current_steps != target_steps ) {
            bool forward = ( target_steps > current_steps );
            set_direction( hw, forward );
            step_pulse( hw );
            current_steps += forward ? 1 : -1;

            // Yield for step_period_us.  vTaskDelay has 1 ms resolution; for
            // rates ≤ 1000 Hz the ms delay is accurate enough.  At higher
            // rates the busy-wait pulse time already dominates anyway.
            TickType_t delay_ms = step_period_us / 1000u;
            vTaskDelay( pdMS_TO_TICKS( delay_ms < 1u ? 1u : delay_ms ) );
        } else {
            // Idle — block until a new command might arrive (10 ms poll)
            vTaskDelay( pdMS_TO_TICKS( 10 ) );
        }
    }
}

// ── Servo controller task ─────────────────────────────────────────────────────
// Reads the latest ground-station and rocket positions from the shared
// depth-1 location queues, computes azimuth and elevation, converts to
// step targets, and overwrites the servo command queues at 10 Hz.
static void servo_ctrl_task( void* )
{
    log_print( "[servo_ctrl] waiting for location fixes...\n" );

    LocationMsg gs  = {};
    LocationMsg rkt = {};
    bool have_gs    = false;
    bool have_rkt   = false;

    for ( ;; ) {
        if ( xQueuePeek( g_gs_location_q,     &gs,  0 ) == pdTRUE ) have_gs  = true;
        if ( xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE ) have_rkt = true;

        if ( have_gs && have_rkt ) {
            rocket_math::Location station { gs.lat,  gs.lon,  gs.alt_m };
            rocket_math::Location rocket  { rkt.lat, rkt.lon, rkt.alt_m };

            double az = rocket_math::GroundStationMath::calculateAzimuth(  station, rocket );
            double el = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            // Convert angles to absolute step counts
            auto az_steps = static_cast<int32_t>( az * ServoCfg::STEPS_PER_DEG );
            auto el_steps = static_cast<int32_t>( el * ServoCfg::STEPS_PER_DEG );

            ServoCmd cmd1 { az_steps, ServoCfg::DEFAULT_STEP_HZ };
            ServoCmd cmd2 { el_steps, ServoCfg::DEFAULT_STEP_HZ };

            xQueueOverwrite( g_servo1_cmd_q, &cmd1 );
            xQueueOverwrite( g_servo2_cmd_q, &cmd2 );
        }

        vTaskDelay( pdMS_TO_TICKS( 100 ) );   // 10 Hz control loop
    }
}

// ── Servo 1 (Zenith, GPIO 0-3) ────────────────────────────────────────────────
static ServoHwCfg s_srv1_hw = {
    .pul_p     = Pins::SRV1_PUL_P,
    .pul_n     = Pins::SRV1_PUL_N,
    .dir_p     = Pins::SRV1_DIR_P,
    .dir_n     = Pins::SRV1_DIR_N,
    .cmd_q_ptr = &g_servo1_cmd_q,
    .tag       = "srv1",
};

static StaticTask_t s_srv1_tcb;
static StackType_t  s_srv1_stack[ 512 ];

void servo1_task_init()
{
    g_servo1_cmd_q = xQueueCreateStatic( 1, sizeof(ServoCmd),
                                          s_srv1_cmd_storage, &s_srv1_cmd_buf );
    configASSERT( g_servo1_cmd_q );

    task_create( servo_hw_task, "srv1", 512, &s_srv1_hw, tskIDLE_PRIORITY + 3,
                  s_srv1_stack, &s_srv1_tcb );
}

// ── Servo 2 (Azimuth, GPIO 4-7) ──────────────────────────────────────────────
static ServoHwCfg s_srv2_hw = {
    .pul_p     = Pins::SRV2_PUL_P,
    .pul_n     = Pins::SRV2_PUL_N,
    .dir_p     = Pins::SRV2_DIR_P,
    .dir_n     = Pins::SRV2_DIR_N,
    .cmd_q_ptr = &g_servo2_cmd_q,
    .tag       = "srv2",
};

static StaticTask_t s_srv2_tcb;
static StackType_t  s_srv2_stack[ 512 ];

void servo2_task_init()
{
    g_servo2_cmd_q = xQueueCreateStatic( 1, sizeof(ServoCmd),
                                          s_srv2_cmd_storage, &s_srv2_cmd_buf );
    configASSERT( g_servo2_cmd_q );

    task_create( servo_hw_task, "srv2", 512, &s_srv2_hw, tskIDLE_PRIORITY + 3,
                  s_srv2_stack, &s_srv2_tcb );
}

// ── Servo controller ──────────────────────────────────────────────────────────
static StaticTask_t s_ctrl_tcb;
static StackType_t  s_ctrl_stack[ 512 ];

void servo_ctrl_task_init()
{
    task_create( servo_ctrl_task, "srv_ctrl", 512, nullptr, tskIDLE_PRIORITY + 2,
                  s_ctrl_stack, &s_ctrl_tcb );
}
