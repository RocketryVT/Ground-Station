#include "cl57te.hpp"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include <math.h>

// -----------------------------------------------------------------------------
// Hardware timer ISR  –  runs in IRQ context, no FreeRTOS calls allowed
// -----------------------------------------------------------------------------

static bool step_isr_cb( repeating_timer_t* rt )
{
    auto* d = static_cast<Cl57te*>( rt->user_data );
    return d->isr_tick();
}

bool Cl57te::isr_tick()
{
    if ( remaining_ <= 0 ) return false;

    // Differential step pulse: PUL+=HIGH/PUL-=LOW for 2 µs, then restore idle.
    gpio_put( cfg_.pul, true );
    if ( cfg_.pul_n != INVALID_PIN ) gpio_put( cfg_.pul_n, false );
    busy_wait_us_32( 5 );   // spec minimum 2.5 µs; 5 µs gives 2× margin
    gpio_put( cfg_.pul, false );
    if ( cfg_.pul_n != INVALID_PIN ) gpio_put( cfg_.pul_n, true );

    --remaining_;
    return remaining_ > 0;      // false -> timer self-cancels
}

// -----------------------------------------------------------------------------
// Cl57te::init
// -----------------------------------------------------------------------------

void Cl57te::init( const Config& cfg )
{
    cfg_ = cfg;

    // PUL+ – output, idle LOW
    gpio_init( cfg_.pul );
    gpio_set_dir( cfg_.pul, GPIO_OUT );
    gpio_put( cfg_.pul, false );

    // PUL- – output, idle HIGH (complement; skipped if single-ended)
    if ( cfg_.pul_n != INVALID_PIN ) {
        gpio_init( cfg_.pul_n );
        gpio_set_dir( cfg_.pul_n, GPIO_OUT );
        gpio_put( cfg_.pul_n, true );
    }

    // DIR+ – output, idle LOW (positive direction)
    gpio_init( cfg_.dir );
    gpio_set_dir( cfg_.dir, GPIO_OUT );
    gpio_put( cfg_.dir, false );

    // DIR- – output, idle HIGH (complement; skipped if single-ended)
    if ( cfg_.dir_n != INVALID_PIN ) {
        gpio_init( cfg_.dir_n );
        gpio_set_dir( cfg_.dir_n, GPIO_OUT );
        gpio_put( cfg_.dir_n, true );
    }

    // ENA – output, HIGH disables the drive; start disabled (safe)
    if ( cfg_.ena != INVALID_PIN ) {
        gpio_init( cfg_.ena );
        gpio_set_dir( cfg_.ena, GPIO_OUT );
        gpio_put( cfg_.ena, true );     // HIGH = disabled
    }
    enabled_ = false;

    // ALM – input with pull-up; drive pulls LOW on fault
    if ( cfg_.alm != INVALID_PIN ) {
        gpio_init( cfg_.alm );
        gpio_set_dir( cfg_.alm, GPIO_IN );
        gpio_pull_up( cfg_.alm );
    }

    // Pre-compute steps/degree for angle conversions
    float motor_revs_per_output_rev = cfg_.gear_ratio;
    float output_steps_per_rev = static_cast<float>( cfg_.pulses_per_rev )
                                 * motor_revs_per_output_rev;
    steps_per_deg_ = output_steps_per_rev / 360.0f;

    remaining_ = 0;
    pos_steps_ = 0;
}

// -----------------------------------------------------------------------------
// Cl57te::enable / disable
// -----------------------------------------------------------------------------

void Cl57te::enable()
{
    if ( cfg_.ena != INVALID_PIN )
        gpio_put( cfg_.ena, false );    // LOW = ENA inactive = drive ENABLED
    enabled_ = true;
    // Caller is responsible for waiting ≥ 200 ms (vTaskDelay) before the
    // first start_move() call — sleep here would busy-wait in RTOS context.
}

void Cl57te::disable()
{
    stop();
    if ( cfg_.ena != INVALID_PIN )
        gpio_put( cfg_.ena, true );     // HIGH = ENA active = drive DISABLED
    enabled_ = false;
}

// -----------------------------------------------------------------------------
// Cl57te::start_move
// -----------------------------------------------------------------------------

bool Cl57te::start_move( int32_t n_steps, uint32_t step_hz )
{
    if ( !enabled_ || is_faulted() ) return false;
    if ( n_steps == 0 )              return true;

    // Abort any in-progress move
    if ( remaining_ > 0 ) stop();

    // Set direction first, then observe DIR setup time (≥ 5 µs)
    bool forward = ( n_steps > 0 );
    gpio_put( cfg_.dir, forward );
    if ( cfg_.dir_n != INVALID_PIN ) gpio_put( cfg_.dir_n, !forward );
    busy_wait_us_32( 10 );  // 10 µs ≥ the 5 µs minimum

    // Choose step rate
    if ( step_hz == 0 ) {
        float dps = cfg_.default_speed_dps;
        step_hz = static_cast<uint32_t>( dps * steps_per_deg_ );
    }
    if ( step_hz < 1 )        step_hz = 1;
    float max_hz = cfg_.max_speed_dps * steps_per_deg_;
    if ( step_hz > static_cast<uint32_t>( max_hz ) )
        step_hz = static_cast<uint32_t>( max_hz );

    int64_t period_us = -( static_cast<int64_t>( 1'000'000 ) / step_hz );

    remaining_ = ( n_steps > 0 ) ? n_steps : -n_steps;
    add_repeating_timer_us( period_us, step_isr_cb, this, &timer );
    return true;
}

// -----------------------------------------------------------------------------
// Cl57te::stop
// -----------------------------------------------------------------------------

void Cl57te::stop()
{
    if ( remaining_ > 0 ) {
        cancel_repeating_timer( &timer );
        remaining_ = 0;
    }
}

// -----------------------------------------------------------------------------
// Cl57te::angle_deg
// -----------------------------------------------------------------------------

float Cl57te::angle_deg() const
{
    return static_cast<float>( pos_steps_ ) / steps_per_deg_;
}
