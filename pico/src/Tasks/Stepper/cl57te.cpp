#include "cl57te.hpp"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include <math.h>

static constexpr uint32_t kDirSetupUs = 100;
static constexpr uint32_t kStopIdleUs = 1000;
static constexpr uint32_t kPulseLowUs = 5;

static float wrap_360_local( float deg )
{
    while ( deg < 0.0f )    deg += 360.0f;
    while ( deg >= 360.0f ) deg -= 360.0f;
    return deg;
}

static float normalize_delta_180( float deg )
{
    while ( deg > 180.0f )   deg -= 360.0f;
    while ( deg <= -180.0f ) deg += 360.0f;
    return deg;
}

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
    if ( !timer_running_ ) return false;

    const int32_t delta_steps = target_steps_ - pos_steps_;
    if ( delta_steps == 0 ) {
        timer_running_ = false;
        active_step_sign_ = 0;
        gpio_put( cfg_.pul_n, true );
        return false;
    }

    const int8_t desired_sign = ( delta_steps > 0 ) ? 1 : -1;
    if ( active_step_sign_ != desired_sign ) {
        const bool dir_level = dir_level_for_steps( desired_sign );
        const bool direction_changed =
            have_last_dir_level_ && ( dir_level != last_dir_level_ );

        gpio_put( cfg_.pul_n, true );
        gpio_put( cfg_.dir_n, dir_level );
        last_dir_level_ = dir_level;
        have_last_dir_level_ = true;
        active_step_sign_ = desired_sign;

        const uint32_t guard_us = direction_changed ? kStopIdleUs : kDirSetupUs;
        dir_hold_until_us_ = time_us_32() + guard_us;
        return true;
    }

    if ( (int32_t)( time_us_32() - dir_hold_until_us_ ) < 0 ) {
        return true;
    }

    // PUL- is active-low: idle HIGH, pulse LOW, then return HIGH.
    gpio_put( cfg_.pul_n, false );
    busy_wait_us_32( kPulseLowUs );
    gpio_put( cfg_.pul_n, true );

    pos_steps_ = pos_steps_ + active_step_sign_;
    return true;
}

// -----------------------------------------------------------------------------
// Cl57te::init
// -----------------------------------------------------------------------------

void Cl57te::init( const Config& cfg )
{
    cfg_ = cfg;

    gpio_init( cfg_.pul_n );
    gpio_set_dir( cfg_.pul_n, GPIO_OUT );
    gpio_put( cfg_.pul_n, true );     // PUL- idle HIGH

    gpio_init( cfg_.dir_n );
    gpio_set_dir( cfg_.dir_n, GPIO_OUT );
    gpio_put( cfg_.dir_n, true );     // DIR- idle = positive direction
    have_last_dir_level_ = true;
    last_dir_level_ = true;

    // Pre-compute steps/degree for angle conversions
    float motor_revs_per_output_rev = cfg_.gear_ratio;
    float output_steps_per_rev = static_cast<float>( cfg_.pulses_per_rev )
                                 * motor_revs_per_output_rev;
    steps_per_deg_ = output_steps_per_rev / 360.0f;

    pos_steps_ = 0;
    target_steps_ = 0;
    active_step_sign_ = 0;
    timer_running_ = false;
    dir_hold_until_us_ = 0;
}

// -----------------------------------------------------------------------------
// Angle reference and targeting
// -----------------------------------------------------------------------------

int32_t Cl57te::angle_to_steps( float angle_deg ) const
{
    return static_cast<int32_t>( roundf( angle_deg * steps_per_deg_ ) );
}

void Cl57te::set_current_angle( float angle_deg )
{
    stop();
    const int32_t steps = angle_to_steps( angle_deg );
    const uint32_t irq_state = save_and_disable_interrupts();
    pos_steps_ = steps;
    target_steps_ = steps;
    restore_interrupts( irq_state );
}

int32_t Cl57te::target_steps_for_angle( float target_angle_deg,
                                        bool shortest_path ) const
{
    if ( !shortest_path ) {
        return angle_to_steps( target_angle_deg );
    }

    const float current_angle = angle_deg();
    const float current_wrapped = wrap_360_local( current_angle );
    const float target_wrapped = wrap_360_local( target_angle_deg );
    const float delta = normalize_delta_180( target_wrapped - current_wrapped );
    return angle_to_steps( current_angle + delta );
}

uint32_t Cl57te::step_hz_for_speed( float speed_dps ) const
{
    float dps = ( speed_dps > 0.0f ) ? speed_dps : cfg_.default_speed_dps;
    if ( dps > cfg_.max_speed_dps ) dps = cfg_.max_speed_dps;

    uint32_t step_hz = static_cast<uint32_t>( dps * steps_per_deg_ );
    return step_hz < 1u ? 1u : step_hz;
}

bool Cl57te::set_angle( float target_angle_deg,
                        float speed_dps,
                        bool shortest_path )
{
    const int32_t target_steps =
        target_steps_for_angle( target_angle_deg, shortest_path );

    const uint32_t irq_state = save_and_disable_interrupts();
    target_steps_ = target_steps;
    restore_interrupts( irq_state );

    return ensure_timer_running( step_hz_for_speed( speed_dps ) );
}

// -----------------------------------------------------------------------------
// Cl57te::start_move
// -----------------------------------------------------------------------------

bool Cl57te::dir_level_for_steps( int32_t n_steps ) const
{
    bool forward = ( n_steps > 0 );
    return !forward;
}

bool Cl57te::dir_gpio_level() const
{
    return gpio_get( cfg_.dir_n );
}

bool Cl57te::start_move( int32_t n_steps, uint32_t step_hz )
{
    if ( n_steps == 0 ) return true;

    const uint32_t irq_state = save_and_disable_interrupts();
    target_steps_ = pos_steps_ + n_steps;
    restore_interrupts( irq_state );

    return ensure_timer_running( step_hz );
}

bool Cl57te::ensure_timer_running( uint32_t step_hz )
{
    if ( target_steps_ == pos_steps_ ) return true;

    if ( timer_running_ ) return true;

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

    const uint32_t irq_state = save_and_disable_interrupts();
    if ( target_steps_ == pos_steps_ ) {
        restore_interrupts( irq_state );
        return true;
    }
    timer_running_ = true;
    active_step_sign_ = 0;
    dir_hold_until_us_ = 0;
    restore_interrupts( irq_state );

    if ( !add_repeating_timer_us( period_us, step_isr_cb, this, &timer ) ) {
        const uint32_t fail_irq_state = save_and_disable_interrupts();
        timer_running_ = false;
        active_step_sign_ = 0;
        restore_interrupts( fail_irq_state );
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Cl57te::stop
// -----------------------------------------------------------------------------

void Cl57te::stop()
{
    const bool was_running = timer_running_;
    const uint32_t irq_state = save_and_disable_interrupts();
    target_steps_ = pos_steps_;
    timer_running_ = false;
    active_step_sign_ = 0;
    restore_interrupts( irq_state );

    if ( was_running ) {
        cancel_repeating_timer( &timer );
    }

    // Always leave PUL- idle HIGH, even if stop() is called after the timer has
    // already self-cancelled. This gives the next direction change a known
    // electrical starting state.
    gpio_put( cfg_.pul_n, true );
    busy_wait_us_32( kStopIdleUs );
}

bool Cl57te::is_moving() const
{
    return timer_running_ || target_steps_ != pos_steps_;
}

// -----------------------------------------------------------------------------
// Cl57te::angle_deg
// -----------------------------------------------------------------------------

float Cl57te::angle_deg() const
{
    return static_cast<float>( pos_steps_ ) / steps_per_deg_;
}

float Cl57te::wrapped_angle_deg() const
{
    return wrap_360_local( angle_deg() );
}
