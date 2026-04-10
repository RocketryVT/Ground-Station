#include "servo_task.hpp"
#include "shared.hpp"

#include "math_utils/GroundStationMath.h"

#include "hardware/gpio.h"
#include "pico/time.h"

#include <algorithm>   // std::clamp
#include <cmath>       // std::abs

// -- Command queue backing storage ---------------------------------------------
QueueHandle_t g_servo1_cmd_q = nullptr;
QueueHandle_t g_servo2_cmd_q = nullptr;

static StaticQueue_t s_srv1_cmd_buf;
static uint8_t       s_srv1_cmd_storage[ sizeof(ServoCmd) ];  // depth 1

static StaticQueue_t s_srv2_cmd_buf;
static uint8_t       s_srv2_cmd_storage[ sizeof(ServoCmd) ];  // depth 1

// -- Hardware task config -------------------------------------------------------
struct ServoHwCfg {
    uint           pul_p;      // PUL+ output  (required)
    uint           pul_n;      // PUL- output  (required)
    uint           dir_p;      // DIR+ output  (required)
    uint           dir_n;      // DIR- output  (required)
    uint           ena;        // ENA+ output  (INVALID_PIN if unused; NC = drive enabled)
    uint           alm;        // ALM input    (INVALID_PIN if unused; LOW = fault)
    QueueHandle_t* cmd_q_ptr;
    const char*    tag;
};

// -- Drive enable/disable helpers ----------------------------------------------
// ENA+: HIGH (4.5-24 V) = disabled, LOW (0-0.5 V) = enabled.
// ENA- is tied to GND, so the GPIO level maps directly.
static inline void drive_enable( const ServoHwCfg* hw )
{
    if ( hw->ena != INVALID_PIN )
        gpio_put( hw->ena, 0u );
}

static inline void drive_disable( const ServoHwCfg* hw )
{
    if ( hw->ena != INVALID_PIN )
        gpio_put( hw->ena, 1u );
}

// -- Step pulse helpers --------------------------------------------------------
// Differential outputs: PUL-/DIR- are always driven as the complement of +.
// CL57TE latches on the falling edge of PUL by default.
static inline void set_direction( const ServoHwCfg* hw, bool forward )
{
    gpio_put( hw->dir_p, forward ? 1u : 0u );
    gpio_put( hw->dir_n, forward ? 0u : 1u );
    sleep_us( ServoCfg::DIR_SETUP_US );
}

static inline void step_pulse( const ServoHwCfg* hw )
{
    gpio_put( hw->pul_p, 1u );  gpio_put( hw->pul_n, 0u );
    sleep_us( ServoCfg::PULSE_US );
    gpio_put( hw->pul_p, 0u );  gpio_put( hw->pul_n, 1u );  // falling edge on PUL+
    sleep_us( ServoCfg::PULSE_US );
}

// -- Generic step/dir hardware task -------------------------------------------
// One instance runs per servo axis.  The task owns its GPIO pins and executes
// step pulses toward the target position held in the command queue.
//
// - xQueuePeek() is non-destructive: the latest overwritten target persists.
// - ALM pin is checked each iteration; a fault is logged once per event edge.
// - ENA is asserted (LOW) at startup and left there; drive_disable() is
//   available but not called in normal operation.
static void servo_hw_task( void* arg )
{
    const ServoHwCfg* hw = static_cast<const ServoHwCfg*>( arg );

    // -- Init differential PUL and DIR outputs -----------------------------
    const uint out_pins[4] = { hw->pul_p, hw->pul_n, hw->dir_p, hw->dir_n };
    for ( uint p : out_pins ) {
        gpio_init( p );
        gpio_set_dir( p, GPIO_OUT );
    }
    // Idle state: PUL low-side asserted, DIR+ low / DIR- high
    gpio_put( hw->pul_p, 0u );  gpio_put( hw->pul_n, 1u );
    gpio_put( hw->dir_p, 0u );  gpio_put( hw->dir_n, 1u );

    // -- Init optional ENA output -------------------------------------------
    if ( hw->ena != INVALID_PIN ) {
        gpio_init( hw->ena );
        gpio_set_dir( hw->ena, GPIO_OUT );
        drive_enable( hw );   // LOW = drive active
    }

    // -- Init optional ALM input --------------------------------------------
    if ( hw->alm != INVALID_PIN ) {
        gpio_init( hw->alm );
        gpio_set_dir( hw->alm, GPIO_IN );
        gpio_pull_up( hw->alm );   // open-collector output — LOW = fault
    }

    log_print( "[%s] CL57TE ready  PUL+=%u PUL-=%u  DIR+=%u DIR-=%u  ENA=%s ALM=%s\n",
               hw->tag,
               hw->pul_p, hw->pul_n, hw->dir_p, hw->dir_n,
               hw->ena != INVALID_PIN ? "wired" : "NC",
               hw->alm != INVALID_PIN ? "wired" : "NC" );

    QueueHandle_t cmd_q = *hw->cmd_q_ptr;

    int32_t  current_steps = 0;
    int32_t  target_steps  = 0;
    uint16_t step_hz       = ServoCfg::DEFAULT_STEP_HZ;
    uint32_t step_period_us = 1'000'000u / step_hz;

    bool alm_prev = false;   // previous fault state for edge detection

    for ( ;; ) {
        // -- Check ALM fault pin (edge-triggered log) -----------------------
        if ( hw->alm != INVALID_PIN ) {
            bool alm_now = !gpio_get( hw->alm );   // LOW = fault
            if ( alm_now && !alm_prev )
                log_print( "[%s] FAULT detected on ALM pin\n", hw->tag );
            else if ( !alm_now && alm_prev )
                log_print( "[%s] fault cleared\n", hw->tag );
            alm_prev = alm_now;
        }

        // -- Consume latest target from queue ------------------------------
        ServoCmd cmd;
        if ( xQueuePeek( cmd_q, &cmd, 0 ) == pdTRUE ) {
            target_steps = cmd.target_steps;
            if ( cmd.step_hz > 0 ) {
                step_hz         = cmd.step_hz;
                step_period_us  = 1'000'000u / step_hz;
            }
        }

        // -- Step toward target --------------------------------------------
        if ( current_steps != target_steps ) {
            bool forward = ( target_steps > current_steps );
            set_direction( hw, forward );
            step_pulse( hw );
            current_steps += forward ? 1 : -1;

            // vTaskDelay has 1 ms resolution.  At step_hz ≤ 1000 the ms delay
            // is accurate enough; at higher rates the pulse busy-wait dominates.
            TickType_t delay_ms = step_period_us / 1000u;
            vTaskDelay( pdMS_TO_TICKS( delay_ms < 1u ? 1u : delay_ms ) );
        } else {
            vTaskDelay( pdMS_TO_TICKS( 10 ) );   // idle poll at 100 Hz
        }
    }
}

// -- Servo controller task -----------------------------------------------------
// Reads ground-station and rocket positions from the shared depth-1 queues,
// computes azimuth and elevation, converts to step targets, and posts to the
// hardware task queues at 10 Hz.
//
// Azimuth uses shortest-path logic: the accumulated target angle may exceed
// 0-360° so the motor always rotates the short way.
// Elevation is clamped to [EL_MIN_DEG, EL_MAX_DEG].
static void servo_ctrl_task( void* )
{
    log_print( "[servo_ctrl] waiting for location fixes...\n" );

    LocationMsg gs  = {};
    LocationMsg rkt = {};
    bool have_gs    = false;
    bool have_rkt   = false;

    double az_target_deg = 0.0;   // accumulated azimuth (may exceed 0-360)

    for ( ;; ) {
        if ( xQueuePeek( g_gs_location_q,     &gs,  0 ) == pdTRUE ) have_gs  = true;
        if ( xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE ) have_rkt = true;

        if ( have_gs && have_rkt ) {
            rocket_math::Location station { gs.lat,  gs.lon,  gs.alt_m };
            rocket_math::Location rocket  { rkt.lat, rkt.lon, rkt.alt_m };

            double raw_az = rocket_math::GroundStationMath::calculateAzimuth(  station, rocket );
            double raw_el = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            // -- Azimuth: shortest angular path ----------------------------
            // Compute the angular delta from the current target and normalise
            // it to [-180, 180] so the motor never spins more than 180°.
            double delta_az = raw_az - az_target_deg;
            while ( delta_az >  180.0 ) delta_az -= 360.0;
            while ( delta_az < -180.0 ) delta_az += 360.0;
            az_target_deg += delta_az;

            // -- Elevation: clamp to mechanical limits ---------------------
            double el = std::clamp( raw_el,
                                    static_cast<double>( ServoCfg::EL_MIN_DEG ),
                                    static_cast<double>( ServoCfg::EL_MAX_DEG ) );

            auto az_steps = static_cast<int32_t>( az_target_deg * ServoCfg::STEPS_PER_DEG );
            auto el_steps = static_cast<int32_t>( el             * ServoCfg::STEPS_PER_DEG );

            ServoCmd cmd_az { az_steps, ServoCfg::DEFAULT_STEP_HZ };
            ServoCmd cmd_el { el_steps, ServoCfg::DEFAULT_STEP_HZ };

            xQueueOverwrite( g_servo2_cmd_q, &cmd_az );   // srv2 = azimuth
            xQueueOverwrite( g_servo1_cmd_q, &cmd_el );   // srv1 = elevation
        }

        vTaskDelay( pdMS_TO_TICKS( 100 ) );   // 10 Hz control loop
    }
}

// -- Servo 1 – Elevation (GPIO 0-3) --------------------------------------------
static ServoHwCfg s_srv1_hw = {
    .pul_p     = Pins::STEP1_PUL,
    .pul_n     = Pins::STEP1_ENA,   // GPIO 1 driven as PUL- complement
    .dir_p     = Pins::STEP1_DIR,
    .dir_n     = Pins::STEP1_ALM,   // GPIO 3 driven as DIR- complement
    .ena       = INVALID_PIN,
    .alm       = INVALID_PIN,
    .cmd_q_ptr = &g_servo1_cmd_q,
    .tag       = "srv1-el",
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

// -- Servo 2 – Azimuth (GPIO 4-7) ----------------------------------------------
static ServoHwCfg s_srv2_hw = {
    .pul_p     = Pins::STEP2_PUL_P,
    .pul_n     = Pins::STEP2_PUL_N,
    .dir_p     = Pins::STEP2_DIR_P,
    .dir_n     = Pins::STEP2_DIR_N,
    .ena       = INVALID_PIN,          // not wired; NC = drive always enabled
    .alm       = INVALID_PIN,          // not wired; assign a spare GPIO to enable fault logging
    .cmd_q_ptr = &g_servo2_cmd_q,
    .tag       = "srv2-az",
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

// -- Servo controller ----------------------------------------------------------
static StaticTask_t s_ctrl_tcb;
static StackType_t  s_ctrl_stack[ 512 ];

void servo_ctrl_task_init()
{
    task_create( servo_ctrl_task, "srv_ctrl", 512, nullptr, tskIDLE_PRIORITY + 2,
                  s_ctrl_stack, &s_ctrl_tcb );
}
