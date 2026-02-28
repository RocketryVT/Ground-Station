#include "servo_task.hpp"
#include "shared.hpp"

#include "math_utils/GroundStationMath.h"

#include "hardware/pwm.h"
#include "hardware/gpio.h"

// ── PWM constants ─────────────────────────────────────────────────────────────
// 125 MHz system clock ÷ 125 clkdiv = 1 MHz counter clock.
// Wrap at 20 000 → period = 20 ms = 50 Hz (standard hobby servo frequency).
// Pulse width counts: 1 000 = 1 ms (full CCW), 1 500 = 1.5 ms (centre),
//                     2 000 = 2 ms (full CW).
static constexpr float    PWM_CLKDIV   = 125.0f;
static constexpr uint32_t PWM_WRAP     = 19999;   // 0-indexed → 20 000 steps
static constexpr uint16_t PULSE_MIN    = 1000;    // 1 ms
static constexpr uint16_t PULSE_CENTER = 1500;    // 1.5 ms
static constexpr uint16_t PULSE_MAX    = 2000;    // 2 ms

// ── Helpers ───────────────────────────────────────────────────────────────────
// Map an angle (degrees) to a PWM pulse-width count, clamped to [min, max].
static uint16_t angle_to_counts( double deg, double deg_min, double deg_max )
{
    if ( deg < deg_min ) deg = deg_min;
    if ( deg > deg_max ) deg = deg_max;
    double t = ( deg - deg_min ) / ( deg_max - deg_min );
    return static_cast<uint16_t>( PULSE_MIN + t * ( PULSE_MAX - PULSE_MIN ) );
}

static void servo_pwm_init()
{
    gpio_set_function( Pins::SERVO_AZIMUTH, GPIO_FUNC_PWM );
    gpio_set_function( Pins::SERVO_ZENITH,  GPIO_FUNC_PWM );

    // GPIO 0 and GPIO 1 share PWM slice 0 (channels A and B).
    uint slice = pwm_gpio_to_slice_num( Pins::SERVO_AZIMUTH );

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv( &cfg, PWM_CLKDIV );
    pwm_config_set_wrap( &cfg, PWM_WRAP );
    pwm_init( slice, &cfg, true );

    // Park both servos at centre on startup
    pwm_set_gpio_level( Pins::SERVO_AZIMUTH, PULSE_CENTER );
    pwm_set_gpio_level( Pins::SERVO_ZENITH,  PULSE_CENTER );
}

// ── Task ─────────────────────────────────────────────────────────────────────
static void servo_task( void* param )
{
    ( void ) param;

    servo_pwm_init();
    log_print( "[servo] PWM 50 Hz ready — GPIO az=%u el=%u\n",
               Pins::SERVO_AZIMUTH, Pins::SERVO_ZENITH );

    LocationMsg gs  = {};
    LocationMsg rkt = {};
    bool have_gs    = false;
    bool have_rkt   = false;

    for ( ;; ) {
        // Peek latest positions — non-destructive, writers use xQueueOverwrite
        if ( xQueuePeek( g_gs_location_q,     &gs,  0 ) == pdTRUE ) have_gs  = true;
        if ( xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE ) have_rkt = true;

        if ( have_gs && have_rkt ) {
            rocket_math::Location station { gs.lat,  gs.lon,  gs.alt_m };
            rocket_math::Location rocket  { rkt.lat, rkt.lon, rkt.alt_m };

            double az = rocket_math::GroundStationMath::calculateAzimuth(  station, rocket );
            double el = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            // Azimuth servo covers 0–180°.  Normalise the full-circle bearing
            // to the nearest servo-reachable angle (wrap >180° back to 0–180°).
            if ( az > 180.0 ) az = 360.0 - az;

            uint16_t az_counts = angle_to_counts( az, 0.0, 180.0 );
            uint16_t el_counts = angle_to_counts( el, 0.0,  90.0 );

            pwm_set_gpio_level( Pins::SERVO_AZIMUTH, az_counts );
            pwm_set_gpio_level( Pins::SERVO_ZENITH,  el_counts );
        } else {
            // No fix yet — hold centre
            pwm_set_gpio_level( Pins::SERVO_AZIMUTH, PULSE_CENTER );
            pwm_set_gpio_level( Pins::SERVO_ZENITH,  PULSE_CENTER );
        }

        vTaskDelay( pdMS_TO_TICKS( 100 ) );   // 10 Hz update — plenty for tracking
    }
}

static StaticTask_t s_servo_tcb;
static StackType_t  s_servo_stack[ 1024 ];

void servo_task_init()
{
    configASSERT( xTaskCreateStatic( servo_task, "servo", 1024,
                                      NULL, tskIDLE_PRIORITY + 2,
                                      s_servo_stack, &s_servo_tcb ) );
}
