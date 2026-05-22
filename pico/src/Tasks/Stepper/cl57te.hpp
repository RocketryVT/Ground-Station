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
    // start_move: begin stepping toward |n_steps| from current position.
    //   n_steps > 0  -> positive direction (DIR- LOW)
    //   n_steps < 0  -> negative direction (DIR- HIGH)
    //   step_hz      -> step pulse rate; 0 = use default from Config
    //
    // Starts a repeating hardware timer that generates step pulses via isr_tick().
    // Returns false only if the timer could not be started.
    bool start_move( int32_t n_steps, uint32_t step_hz = 0 );

    // Abort the current move (cancels the timer). Position remains at the last
    // emitted step.
    void stop();

    bool is_moving() const  { return remaining_ > 0; }

    // -- ISR entry point -------------------------------------------------------
    // Returns true to keep the repeating timer alive; false to cancel it.
    // Called from hardware-timer IRQ — keep it short, no FreeRTOS calls.
    bool isr_tick();

    // -- Position --------------------------------------------------------------
    // pos_steps_ is advanced by the timer ISR for each emitted step. It is the
    // commanded mechanical position, not an encoder measurement.
    int32_t  pos_steps() const  { return pos_steps_; }
    float    angle_deg()  const;

    // Called by task once is_moving() becomes false to finalise pos_steps_.
    void commit_position( int32_t pos ) { pos_steps_ = pos; }

    float steps_per_deg() const { return steps_per_deg_; }
    uint  dir_pin() const { return cfg_.dir_n; }
    bool  dir_level_for_steps( int32_t n_steps ) const;
    bool  dir_gpio_level() const;

    // -- Public timer handle (needed for add_repeating_timer_us user_data) ----
    repeating_timer_t timer;

private:
    Config   cfg_{};
    volatile int32_t  pos_steps_ = 0;
    volatile int8_t   step_sign_ = 1;

    float    steps_per_deg_ = 1.0f;   // cached at init

    // Modified by ISR, read by task — volatile is sufficient here because
    // the task only reads (never writes while the timer is running).
    volatile int32_t  remaining_ = 0;
};
