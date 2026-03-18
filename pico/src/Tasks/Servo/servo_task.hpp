#pragma once

#include "FreeRTOS.h"
#include "queue.h"

// ── Servo step/dir command ────────────────────────────────────────────────────
// Send to g_servo1_cmd_q or g_servo2_cmd_q from any task.
// The step task owns its GPIO pins; callers never touch them directly.
//
//   target_steps  – absolute position in steps from the home/zero position.
//   step_hz       – step pulse rate (steps/sec). 0 = keep current rate.
//
// The step task uses xQueuePeek (non-destructive) so the controller can
// xQueueOverwrite at 10 Hz and the step task always sees the latest target
// without the controller needing to repost after every consumed message.
struct ServoCmd {
    int32_t  target_steps;
    uint16_t step_hz;
};

// ── Command queues (depth 1 – always holds the latest target) ─────────────────
extern QueueHandle_t g_servo1_cmd_q;   // Servo 1 – Zenith  (GPIO 0-3)
extern QueueHandle_t g_servo2_cmd_q;   // Servo 2 – Azimuth (GPIO 4-7)

// ── Servo drive configuration ─────────────────────────────────────────────────
namespace ServoCfg {
    // Steps per revolution of the servo output shaft.
    // Adjust to match the drive's electronic gearing setting.
    static constexpr float    STEPS_PER_REV   = 1000.0f;
    static constexpr float    STEPS_PER_DEG   = STEPS_PER_REV / 360.0f;

    // Default step rate when none is specified in the command.
    static constexpr uint16_t DEFAULT_STEP_HZ = 200;    // steps/sec

    // Minimum pulse width for PUL and DIR setup time (µs).
    static constexpr uint32_t PULSE_US        = 5;
    static constexpr uint32_t DIR_SETUP_US    = 5;
}

// ── Init functions ─────────────────────────────────────────────────────────────
void servo1_task_init();     // hardware task – owns GPIO 0-3, drains g_servo1_cmd_q
void servo2_task_init();     // hardware task – owns GPIO 4-7, drains g_servo2_cmd_q
void servo_ctrl_task_init(); // controller – reads location queues, posts ServoCmd
