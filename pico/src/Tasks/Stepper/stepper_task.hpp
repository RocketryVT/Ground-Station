#pragma once

// -----------------------------------------------------------------------------
// Stepper axis tasks  –  one task per axis (azimuth + elevation)
//
// Motor hardware: CL57TE closed-loop stepper drive
//
// Motor configuration (compile-time, adjust to match drive DIP switches and
// physical gearing):
//   STEP_PULSES_PER_REV  – drive DIP switch resolution (default 1600 p/rev)
//   GEAR_RATIO           – mechanical reduction on each axis
//
// Command interface:
//   Write a StepperCmd to g_stepper_az_cmd_q or g_stepper_zen_cmd_q using
//   xQueueOverwrite().  The task always acts on the latest value.
//   Set .stop = true to abort motion immediately.
//   STEP2 command angles use elevation convention: 0° = horizon, 90° = up.
//
// Status interface:
//   Peek g_stepper_az_status_q / g_stepper_zen_status_q (depth-1 overwrite)
//   to read the latest StepperStatus without consuming it.
//
// Home position:
//   At start-up the drive has no knowledge of absolute position; pos=0 is
//   defined as wherever the motor shaft was when the driver initialized.
//   Cl57te::zero() / set_current_angle() can later be called from a homing or
//   calibration source to redefine that software reference.
// -----------------------------------------------------------------------------

#include "FreeRTOS.h"
#include "queue.h"

// Sent TO the stepper tasks
struct StepperCmd {
    float  target_angle_deg;    // absolute angle in degrees (az: 0-360; el: 0 horizon, 90 up)
    float  speed_dps;           // desired speed (deg/s); 0 = use default
    bool   stop;                // true -> abort move
};

// Published BY the stepper tasks (depth-1 overwrite, use xQueuePeek)
struct StepperStatus {
    float    angle_deg;         // continuous current software angle in degrees
    bool     moving;            // currently executing a move
    bool     faulted;           // ALM pin asserted
    bool     enabled;           // motor is energised
    uint64_t timestamp_us;
};

// -- Command queues (depth 1, use xQueueOverwrite to send) --------------------
extern QueueHandle_t g_stepper_az_cmd_q;    // Azimuth  (STEP1 – GPIO 4/5)
extern QueueHandle_t g_stepper_zen_cmd_q;   // Elevation (STEP2 – GPIO 6/7)

// -- Status queues (depth 1, use xQueuePeek to read) --------------------------
extern QueueHandle_t g_stepper_az_status_q;
extern QueueHandle_t g_stepper_zen_status_q;

// -- Init ----------------------------------------------------------------------
void stepper_az_task_init();    // azimuth axis  (STEP1 – GPIO 4/5)
void stepper_zen_task_init();   // elevation axis (STEP2 – GPIO 6/7)
void stepper_state_task_init(); // publishes antenna/state from axis status
void stepper_ctrl_task_init();  // controller: reads location queues -> posts StepperCmd
