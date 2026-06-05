#pragma once

#include "hardware/gpio.h"
#include "pico/time.h"
#include "pico/types.h"
#include <stdint.h>

// -----------------------------------------------------------------------------
// Cl57te  –  CL57TE closed-loop stepper drive hardware abstraction
//
// Hardware wiring for this ground-station PCB:
//   PUL+  → +5V                              PUL-      ← Pico GPIO
//   DIR+  → +5V                              DIR-      ← Pico GPIO
//   ENA and ALM are not connected.
//
// The CL57TE opto inputs are therefore active-low:
//   PUL- idle HIGH, step pulse LOW for >= 2.5 us, then HIGH
//   DIR- LOW/HIGH selects direction and stays steady during the move
//
// The drive clocks a step on every rising edge of (PUL+ − PUL-).
// Step resolution (pulses/rev) is set by the drive's DIP switches.
//
// Timing requirements:
//   • PUL high/low ≥ 1 µs   – driver generates 5 µs low pulses
//   • DIR setup ≥ 5 µs before first PUL after a direction change
//
// The driver chases a mutable target position. The owning task can update the
// target while the timer ISR is stepping, and the ISR will reverse direction
// after observing a conservative direction-latch guard time.
//
// Thread safety:
//   isr_tick()  is called from the hardware-timer ISR.
//   All other methods must be called only from the owning FreeRTOS task.
// -----------------------------------------------------------------------------

class Cl57te {
public:
    // -- Hardware configuration ------------------------------------------------
    struct Config {
        uint    pul_n;          // PUL- GPIO, active-low step pulse output
        uint    dir_n;          // DIR- GPIO, active-low direction output

        uint32_t pulses_per_rev;   // pulses/rev as programmed in drive DIP switches
        float    gear_ratio;       // reduction ratio = motor_revs / output_shaft_revs
                                   // (e.g. 10.0 for a 10:1 planetary gearbox, 1.0 = direct drive)

        float    max_speed_dps;    // hard ceiling on degrees/sec
        float    default_speed_dps;
    };

    // -- Lifecycle -------------------------------------------------------------
    void init( const Config& cfg );

    // -- Motion ----------------------------------------------------------------
    // Define the current mechanical position in the driver's software reference
    // frame. Use zero() when a homing/calibration source says "this is zero".
    void zero() { set_current_angle( 0.0f ); }
    void set_current_angle( float angle_deg );

    // Command an absolute angle in degrees. When shortest_path is true, the
    // driver chooses the nearest equivalent target modulo 360 while preserving
    // its continuous turn count. Example: at 350 deg, set_angle(0) moves +10 deg.
    bool set_angle( float target_angle_deg,
                    float speed_dps = 0.0f,
                    bool shortest_path = true );

    // start_move: set a relative target |n_steps| from current position.
    //   n_steps > 0  -> positive direction (DIR- LOW)
    //   n_steps < 0  -> negative direction (DIR- HIGH)
    //   step_hz      -> step pulse rate; 0 = use default from Config
    //
    // Starts the repeating hardware timer when needed. If the timer is already
    // running, this updates the target without stopping pulse generation.
    bool start_move( int32_t n_steps, uint32_t step_hz = 0 );

    // Abort the current move (cancels the timer). Position remains at the last
    // emitted step.
    void stop();

    bool is_moving() const;

    // -- ISR entry point -------------------------------------------------------
    // Returns true to keep the repeating timer alive; false to cancel it.
    // Called from hardware-timer IRQ — keep it short, no FreeRTOS calls.
    bool isr_tick();

    // -- Position --------------------------------------------------------------
    // pos_steps_ is advanced by the timer ISR for each emitted step. It is the
    // commanded mechanical position, not an encoder measurement.
    int32_t  pos_steps() const  { return pos_steps_; }
    int32_t  target_steps() const { return target_steps_; }
    float    angle_deg()  const;
    float    wrapped_angle_deg() const;

    float steps_per_deg() const { return steps_per_deg_; }
    uint  dir_pin() const { return cfg_.dir_n; }
    bool  dir_level_for_steps( int32_t n_steps ) const;
    bool  dir_gpio_level() const;
    bool  last_dir_level() const { return last_dir_level_; }

    // -- Public timer handle (needed for add_repeating_timer_us user_data) ----
    repeating_timer_t timer;

private:
    Config   cfg_{};
    volatile int32_t  pos_steps_ = 0;
    volatile int32_t  target_steps_ = 0;
    volatile int8_t   active_step_sign_ = 0;
    volatile bool     timer_running_ = false;
    volatile uint32_t dir_hold_until_us_ = 0;

    float    steps_per_deg_ = 1.0f;   // cached at init

    volatile bool     have_last_dir_level_ = false;
    volatile bool     last_dir_level_ = true;

    int32_t angle_to_steps( float angle_deg ) const;
    int32_t target_steps_for_angle( float target_angle_deg,
                                    bool shortest_path ) const;
    uint32_t step_hz_for_speed( float speed_dps ) const;
    bool ensure_timer_running( uint32_t step_hz );
};
