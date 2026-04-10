#pragma once

#include "FreeRTOS.h"
#include "queue.h"

// -- Sentinel for optional GPIO pins ------------------------------------------
// Pass INVALID_PIN for ena / alm if the signal is not wired.
static constexpr uint INVALID_PIN = 0xFFu;

// -- Servo step/dir command ----------------------------------------------------
// Send to g_servo1_cmd_q or g_servo2_cmd_q from any task via xQueueOverwrite().
// The hardware task owns its GPIO pins; callers never touch them directly.
//
//   target_steps  – absolute position in steps from the home/zero position.
//   step_hz       – step pulse rate (steps/sec). 0 = keep current rate.
struct ServoCmd {
    int32_t  target_steps;
    uint16_t step_hz;
};

// -- Command queues (depth 1 – always holds the latest target) -----------------
extern QueueHandle_t g_servo1_cmd_q;   // Servo 1 – Elevation (GPIO 0-3)
extern QueueHandle_t g_servo2_cmd_q;   // Servo 2 – Azimuth   (GPIO 4-7)

// -- CL57TE drive configuration ------------------------------------------------
namespace ServoCfg {

    // Microstep resolution — must match DIP switches SW1-SW4.
    // Factory default: 1600 steps/rev (SW1=ON SW2=OFF SW3=ON SW4=ON).
    static constexpr float MICROSTEPS_PER_REV = 1600.0f;

    // Mechanical gear ratio between the motor shaft and the antenna output shaft.
    // Example: a 50:1 worm gear -> set to 50.0f.
    static constexpr float GEAR_RATIO = 1.0f;

    // Derived: steps per degree of antenna rotation.
    static constexpr float STEPS_PER_DEG =
        (MICROSTEPS_PER_REV * GEAR_RATIO) / 360.0f;

    // Default step rate when none is specified in the command.
    static constexpr uint16_t DEFAULT_STEP_HZ = 200;   // steps/sec

    // CL57TE signal timing (single-ended, 5 V logic via level-shifter).
    // Manual: pulse width ≥ 1 µs @ 5 V, DIR setup ≥ 5 µs before PUL edge.
    static constexpr uint32_t PULSE_US     = 5;
    static constexpr uint32_t DIR_SETUP_US = 5;

    // Elevation limits (degrees) — prevents pointing underground or over-zenith.
    static constexpr float EL_MIN_DEG =  0.0f;
    static constexpr float EL_MAX_DEG = 90.0f;

} // namespace ServoCfg

// -- Init functions ------------------------------------------------------------
void servo1_task_init();     // hardware task – owns GPIO 0-3, drains g_servo1_cmd_q
void servo2_task_init();     // hardware task – owns GPIO 4-7, drains g_servo2_cmd_q
void servo_ctrl_task_init(); // controller – reads location queues, posts ServoCmd
