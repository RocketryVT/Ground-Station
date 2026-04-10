#include "stepper_task.hpp"
#include "cl57te.hpp"
#include "shared.hpp"

#include "math_utils/GroundStationMath.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "pico/time.h"

#include <math.h>

// -----------------------------------------------------------------------------
// Motor / drive configuration
//
// CL57TE DIP-switch: 1600 pulses/rev (SW1=ON, SW2=OFF, SW3=ON, SW4=ON).
// Total gear reduction = 10:1 planetary gearbox × belt ratio per axis.
// -----------------------------------------------------------------------------

namespace StepCfg {
    static constexpr uint32_t PULSES_PER_REV       = 1600;

    // Per-axis gear ratios  (10:1 planetary gearbox × belt ratio)
    static constexpr float    AZ_GEAR_RATIO         = 30.0f;   // 10:1 × 3:1 belt  (azimuth)
    static constexpr float    ZEN_GEAR_RATIO        = 50.0f;   // 10:1 × 5:1 belt  (zenith)

    static constexpr float    MAX_SPEED_DPS         = 90.0f;
    static constexpr float    DEFAULT_SPEED_DPS     = 30.0f;

    // Azimuth: shortest-path rotation, no cable-wrap limit yet
    static constexpr float    AZ_MIN_DEG            = -180.0f;
    static constexpr float    AZ_MAX_DEG            =  180.0f;

    // Zenith: mechanical travel limits
    static constexpr float    ZEN_MIN_DEG           =   0.0f;
    static constexpr float    ZEN_MAX_DEG           =  90.0f;
}

// -----------------------------------------------------------------------------
// Queue handles
// -----------------------------------------------------------------------------

QueueHandle_t g_stepper_az_cmd_q    = nullptr;
QueueHandle_t g_stepper_zen_cmd_q   = nullptr;
QueueHandle_t g_stepper_az_status_q = nullptr;
QueueHandle_t g_stepper_zen_status_q = nullptr;

// -----------------------------------------------------------------------------
// Per-axis hardware task context
// -----------------------------------------------------------------------------

struct AxisCfg {
    Cl57te::Config drv;         // hardware pin + motor config
    QueueHandle_t* cmd_q;       // pointer to command queue handle
    QueueHandle_t* status_q;    // pointer to status queue handle
    float min_deg;              // soft travel limit (lower)
    float max_deg;              // soft travel limit (upper)
    const char*    tag;
};

struct AxisContext {
    Cl57te   drv;
    AxisCfg  cfg;
};

static void axis_task_body( void* arg )
{
    auto* ctx = static_cast<AxisContext*>( arg );
    Cl57te&       drv = ctx->drv;
    const AxisCfg& ax = ctx->cfg;

    // Initialise hardware
    drv.init( ax.drv );

    // Enable motor, then wait for startup (CL57TE ≥ 200 ms settling)
    drv.enable();
    vTaskDelay( pdMS_TO_TICKS(200) );

    log_print( "[%s] enabled  steps/deg=%.2f\n",
               ax.tag, drv.steps_per_deg() );

    int32_t  pos_steps    = 0;   // commanded position (what we told the drive)
    int32_t  target_steps = 0;   // latest target from command queue

    for ( ;; ) {
        // -- Fault monitoring -------------------------------------------------
        if ( drv.is_faulted() ) {
            drv.stop();
            drv.disable();
            log_print( "[%s] FAULT detected — re-enable in 5 s\n", ax.tag );
            vTaskDelay( pdMS_TO_TICKS(5000) );
            drv.enable();
            vTaskDelay( pdMS_TO_TICKS(200) );
            log_print( "[%s] re-enabled after fault\n", ax.tag );
        }

        // -- Process latest command -------------------------------------------
        StepperCmd cmd = {};
        if ( xQueuePeek( *ax.cmd_q, &cmd, 0 ) == pdTRUE ) {

            if ( cmd.stop ) {
                drv.stop();
                drv.disable();

            } else if ( drv.is_enabled() ) {
                float clamped = cmd.target_angle_deg;
                if ( clamped < ax.min_deg ) clamped = ax.min_deg;
                if ( clamped > ax.max_deg ) clamped = ax.max_deg;

                int32_t new_target = static_cast<int32_t>(
                    roundf( clamped * drv.steps_per_deg() ) );

                if ( new_target != target_steps ) {
                    target_steps = new_target;
                    if ( drv.is_moving() ) drv.stop();
                }
            }
        }

        // -- Re-enable if a stop command was cleared --------------------------
        if ( !drv.is_enabled() && !drv.is_faulted() ) {
            StepperCmd check = {};
            bool have = ( xQueuePeek( *ax.cmd_q, &check, 0 ) == pdTRUE );
            if ( have && !check.stop ) {
                drv.enable();
                vTaskDelay( pdMS_TO_TICKS(200) );
            }
        }

        // -- Issue move if needed ---------------------------------------------
        if ( drv.is_enabled() && !drv.is_faulted() && !drv.is_moving() ) {
            int32_t delta = target_steps - pos_steps;
            if ( delta != 0 ) {
                StepperCmd c = {};
                xQueuePeek( *ax.cmd_q, &c, 0 );
                uint32_t hz = ( c.speed_dps > 0.0f )
                    ? static_cast<uint32_t>( c.speed_dps * drv.steps_per_deg() )
                    : 0;

                if ( drv.start_move( delta, hz ) ) {
                    pos_steps = target_steps;
                    drv.commit_position( pos_steps );
                }
            }
        }

        // -- Publish status ---------------------------------------------------
        StepperStatus st = {};
        st.angle_deg    = drv.angle_deg();
        st.moving       = drv.is_moving();
        st.faulted      = drv.is_faulted();
        st.enabled      = drv.is_enabled();
        st.timestamp_us = time_us_64();
        xQueueOverwrite( *ax.status_q, &st );

        vTaskDelay( pdMS_TO_TICKS(10) );    // 100 Hz poll rate
    }
}

// -----------------------------------------------------------------------------
// Stepper controller task
// Reads ground-station and rocket location queues, computes az/zenith, posts cmds.
// -----------------------------------------------------------------------------

static void stepper_ctrl_task( void* )
{
    log_print( "[step_ctrl] waiting for location fixes...\n" );

    LocationMsg gs  = {};
    LocationMsg rkt = {};
    bool have_gs    = false;
    bool have_rkt   = false;

    for ( ;; ) {
        if ( g_gs_location_q     && xQueuePeek( g_gs_location_q,     &gs,  0 ) == pdTRUE ) have_gs  = true;
        if ( g_rocket_location_q && xQueuePeek( g_rocket_location_q, &rkt, 0 ) == pdTRUE ) have_rkt = true;

        if ( have_gs && have_rkt ) {
            rocket_math::Location station{ gs.lat,  gs.lon,  gs.alt_m  };
            rocket_math::Location rocket { rkt.lat, rkt.lon, rkt.alt_m };

            double az  = rocket_math::GroundStationMath::calculateAzimuth(  station, rocket );
            double zen = rocket_math::GroundStationMath::calculateElevation( station, rocket );

            // Normalise azimuth to [-180, 180] for shortest-path rotation
            while ( az >  180.0 ) az -= 360.0;
            while ( az < -180.0 ) az += 360.0;

            StepperCmd az_cmd  { static_cast<float>( az  ), 0.0f, false };
            StepperCmd zen_cmd { static_cast<float>( zen ), 0.0f, false };

            if ( g_stepper_az_cmd_q  ) xQueueOverwrite( g_stepper_az_cmd_q,  &az_cmd  );
            if ( g_stepper_zen_cmd_q ) xQueueOverwrite( g_stepper_zen_cmd_q, &zen_cmd );
        }

        vTaskDelay( pdMS_TO_TICKS(100) );   // 10 Hz tracking loop
    }
}

// -----------------------------------------------------------------------------
// Queue backing storage
// -----------------------------------------------------------------------------

static StaticQueue_t s_az_cmd_buf,    s_zen_cmd_buf;
static StaticQueue_t s_az_status_buf, s_zen_status_buf;

static uint8_t s_az_cmd_storage   [ sizeof(StepperCmd)    ];
static uint8_t s_zen_cmd_storage  [ sizeof(StepperCmd)    ];
static uint8_t s_az_status_storage[ sizeof(StepperStatus) ];
static uint8_t s_zen_status_storage[ sizeof(StepperStatus) ];

// -----------------------------------------------------------------------------
// Azimuth axis  (STEP1 – PUL+ GPIO 4, PUL- GPIO 5, DIR+ GPIO 6, DIR- GPIO 7)
// 10:1 planetary gearbox × 3:1 belt = 30:1 total, travel ±180°
// -----------------------------------------------------------------------------

static AxisContext s_az_ctx;

static StaticTask_t s_az_tcb;
static StackType_t  s_az_stack[ 512 ];

void stepper_az_task_init()
{
    g_stepper_az_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_az_cmd_storage, &s_az_cmd_buf );
    g_stepper_az_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_az_status_storage, &s_az_status_buf );

    s_az_ctx.cfg = AxisCfg {
        .drv = {
            .pul             = Pins::STEP1_PUL,
            .pul_n           = Pins::STEP1_PUL_N,
            .dir             = Pins::STEP1_DIR,
            .dir_n           = Pins::STEP1_DIR_N,
            .ena             = Pins::STEP1_ENA,
            .alm             = Pins::STEP1_ALM,
            .pulses_per_rev  = StepCfg::PULSES_PER_REV,
            .gear_ratio      = StepCfg::AZ_GEAR_RATIO,
            .max_speed_dps   = StepCfg::MAX_SPEED_DPS,
            .default_speed_dps = StepCfg::DEFAULT_SPEED_DPS,
        },
        .cmd_q    = &g_stepper_az_cmd_q,
        .status_q = &g_stepper_az_status_q,
        .min_deg  = StepCfg::AZ_MIN_DEG,
        .max_deg  = StepCfg::AZ_MAX_DEG,
        .tag      = "az",
    };

    task_create( axis_task_body, "step_az", 512, &s_az_ctx,
                 tskIDLE_PRIORITY + 3, s_az_stack, &s_az_tcb );
}

// -----------------------------------------------------------------------------
// Zenith axis  (STEP2 – PUL+ GPIO 12, PUL- GPIO 13, DIR+ GPIO 14, DIR- GPIO 15)
// 10:1 planetary gearbox × 5:1 belt = 50:1 total, travel 0–90°
// -----------------------------------------------------------------------------

static AxisContext s_zen_ctx;

static StaticTask_t s_zen_tcb;
static StackType_t  s_zen_stack[ 512 ];

void stepper_zen_task_init()
{
    g_stepper_zen_cmd_q = xQueueCreateStatic(
        1, sizeof(StepperCmd), s_zen_cmd_storage, &s_zen_cmd_buf );
    g_stepper_zen_status_q = xQueueCreateStatic(
        1, sizeof(StepperStatus), s_zen_status_storage, &s_zen_status_buf );

    s_zen_ctx.cfg = AxisCfg {
        .drv = {
            .pul             = Pins::STEP2_PUL,
            .pul_n           = Pins::STEP2_PUL_N,
            .dir             = Pins::STEP2_DIR,
            .dir_n           = Pins::STEP2_DIR_N,
            .ena             = Pins::STEP2_ENA,
            .alm             = Pins::STEP2_ALM,
            .pulses_per_rev  = StepCfg::PULSES_PER_REV,
            .gear_ratio      = StepCfg::ZEN_GEAR_RATIO,
            .max_speed_dps   = StepCfg::MAX_SPEED_DPS,
            .default_speed_dps = StepCfg::DEFAULT_SPEED_DPS,
        },
        .cmd_q    = &g_stepper_zen_cmd_q,
        .status_q = &g_stepper_zen_status_q,
        .min_deg  = StepCfg::ZEN_MIN_DEG,
        .max_deg  = StepCfg::ZEN_MAX_DEG,
        .tag      = "zen",
    };

    task_create( axis_task_body, "step_zen", 512, &s_zen_ctx,
                 tskIDLE_PRIORITY + 3, s_zen_stack, &s_zen_tcb );
}

// -----------------------------------------------------------------------------
// Controller task
// -----------------------------------------------------------------------------

static StaticTask_t s_ctrl_tcb;
static StackType_t  s_ctrl_stack[ 512 ];

void stepper_ctrl_task_init()
{
    task_create( stepper_ctrl_task, "step_ctrl", 512, nullptr,
                 tskIDLE_PRIORITY + 2, s_ctrl_stack, &s_ctrl_tcb );
}
