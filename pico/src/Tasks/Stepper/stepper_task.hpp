#pragma once

// -----------------------------------------------------------------------------
// Stepper axis tasks  –  one task per axis (azimuth + zenith)
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
//   Set .stop = true to abort motion and disable the drive immediately.
//
// Status interface:
//   Peek g_stepper_az_status_q / g_stepper_zen_status_q (depth-1 overwrite)
//   to read the latest StepperStatus without consuming it.
//
// Home position:
//   At start-up the drive has no knowledge of absolute position; pos=0 is
//   defined as wherever the motor shaft was when the task enabled the drive.
//   Implement a homing sequence externally if needed and reset by sending
//   a command with target_angle_deg = 0 from the known home position.
// -----------------------------------------------------------------------------

#include "FreeRTOS.h"
#include "queue.h"

// Sent TO the stepper tasks
struct StepperCmd {
    float  target_angle_deg;    // absolute angle from home in degrees
    float  speed_dps;           // desired speed (deg/s); 0 = use default
    bool   stop;                // true -> abort move and disable drive
};

// Published BY the stepper tasks (depth-1 overwrite, use xQueuePeek)
struct StepperStatus {
    float    angle_deg;         // current position in degrees
    bool     moving;            // currently executing a move
    bool     faulted;           // ALM pin asserted
    bool     enabled;           // motor is energised
    uint64_t timestamp_us;
};

// -- Command queues (depth 1, use xQueueOverwrite to send) --------------------
extern QueueHandle_t g_stepper_az_cmd_q;    // Azimuth  (STEP1 – GPIO 4-7)
extern QueueHandle_t g_stepper_zen_cmd_q;   // Zenith   (STEP2 – GPIO 12-15)

// -- Status queues (depth 1, use xQueuePeek to read) --------------------------
extern QueueHandle_t g_stepper_az_status_q;
extern QueueHandle_t g_stepper_zen_status_q;

// -- Init ----------------------------------------------------------------------
void stepper_az_task_init();    // azimuth axis  (STEP1 – GPIO 4-7)
void stepper_zen_task_init();   // zenith axis   (STEP2 – GPIO 12-15)
void stepper_ctrl_task_init();  // controller: reads location queues -> posts StepperCmd
