#pragma once

// -- StarlinkClient ------------------------------------------------------------
// Minimal gRPC client for the Starlink dish debug API (192.168.100.1:9200).
//
// Uses raw lwIP TCP with NO_SYS=1 (no sockets/netconn).  Callbacks fire in
// IRQ context; a FreeRTOS binary semaphore synchronises them with the task.
//
// All protobuf field numbers are verified from a live protoset extracted with:
//   python3 extract_protoset.py && grpcurl -plaintext 192.168.100.1:9200 list
//
// Implemented RPCs (SpaceX.API.Device.Device / Handle):
//   Data queries:
//     get_location     -> StarlinkLocation  (Request[1017], Response[1017])
//     get_status       -> StarlinkStatus    (Request[1004], Response[2004])
//     get_device_info  -> StarlinkDeviceInfo(Request[1008], Response[1004])
//     get_time         -> uint64_t unix_ns  (Request[1037], Response[1037])
//     get_config       -> StarlinkConfig    (Request[2011], Response[2011])
//   Commands (no meaningful decoded response):
//     reboot           (Request[1001])
//     stow / unstow    (Request[2002], DishStowRequest{unstow[1]: bool})
//     set_inhibit_gps  (Request[2014], DishInhibitGpsRequest{inhibit_gps[1]: bool})
//     enable_debug_telem (Request[1034], EnableDebugTelemRequest{duration_m[1]: uint32})

#include <lwip/err.h>
#include <stdint.h>
#include <stdbool.h>

// ===============================================================================
// Enumerations (from live protoset)
// ===============================================================================

enum class PositionSource : uint8_t {
    AUTO       = 0,
    NONE       = 1,
    UT_INFO    = 2,
    EXTERNAL   = 3,
    GPS        = 4,
    STARLINK   = 5,
    GNC_FUSED  = 6,
    GNC_BAD_SAT= 7,
    GNC_GPS    = 8,
    GNC_PNT    = 9,
    GNC_STATIC = 10,
};

enum class DishState : uint8_t {
    UNKNOWN    = 0,
    CONNECTED  = 1,
    SEARCHING  = 2,
    BOOTING    = 3,
};

// ===============================================================================
// Result structs
// All fields match the proto field names exactly for easy cross-reference.
// ===============================================================================

// GetLocationResponse + LLAPosition
struct StarlinkLocation {
    double         lat;                    // degrees  WGS-84 (LLAPosition field 1)
    double         lon;                    // degrees  WGS-84 (LLAPosition field 2)
    double         alt;                    // metres MSL      (LLAPosition field 3)
    double         horizontal_speed_mps;   // m/s (field 5)
    double         vertical_speed_mps;     // m/s (field 6)
    double         sigma_m;                // horizontal accuracy, metres (field 4)
    PositionSource source;                 // how position was determined (field 3)
};

// DeviceInfo (DishGetStatusResponse field 1, also GetDeviceInfoResponse.device_info[1])
struct StarlinkDeviceInfo {
    char    id[64];                    // field 1
    char    hardware_version[32];      // field 2
    char    software_version[32];      // field 3
    char    country_code[8];           // field 4
    bool    is_dev;                    // field 7
    bool    is_hitl;                   // field 10 (hardware-in-the-loop test unit)
};

// DishObstructionStats (DishGetStatusResponse field 1004)
struct StarlinkObstructionStats {
    float    fraction_obstructed;                  // field 1  (0.0–1.0)
    float    valid_s;                              // field 4  seconds of valid data
    bool     currently_obstructed;                 // field 5
    float    avg_prolonged_obstruction_duration_s; // field 6
    float    avg_prolonged_obstruction_interval_s; // field 7
    bool     avg_prolonged_obstruction_valid;       // field 8
    float    time_obstructed;                      // field 9  total seconds obstructed
    uint32_t patches_valid;                        // field 10
};

// DishAlerts (DishGetStatusResponse field 1005)
struct StarlinkAlerts {
    bool motors_stuck;                   // field 1
    bool thermal_shutdown;               // field 2
    bool thermal_throttle;               // field 3
    bool unexpected_location;            // field 4
    bool mast_not_near_vertical;         // field 5
    bool slow_ethernet_speeds;           // field 6
    bool roaming;                        // field 7
    bool install_pending;                // field 8
    bool is_heating;                     // field 9
    bool power_supply_thermal_throttle;  // field 10
    bool is_power_save_idle;             // field 11
    bool lower_signal_than_predicted;    // field 17
    bool slow_ethernet_speeds_100;       // field 18
    bool obstruction_map_reset;          // field 19
    bool dish_water_detected;            // field 20
    bool router_water_detected;          // field 21
};

// DishReadyStates (DishGetStatusResponse field 1019)
struct StarlinkReadyStates {
    bool cady;   // field 1
    bool scp;    // field 2
    bool l1l2;   // field 3
    bool xphy;   // field 4
    bool aap;    // field 5
    bool rf;     // field 6
};

// DishGetStatusResponse aggregate
struct StarlinkStatus {
    // -- Device identity (field 1 -> DeviceInfo, field 2 -> DeviceState)
    StarlinkDeviceInfo device_info;
    uint64_t           uptime_s;                        // DeviceState.uptime_s[1]

    // -- Connectivity (all float, wire type 5)
    float   pop_ping_latency_ms;                        // field 1009
    float   pop_ping_drop_rate;                         // field 1003
    float   downlink_throughput_bps;                    // field 1007
    float   uplink_throughput_bps;                      // field 1008
    float   seconds_to_first_nonempty_slot;             // field 1002

    // -- Pointing
    float   boresight_azimuth_deg;                      // field 1011
    float   boresight_elevation_deg;                    // field 1012

    // -- GPS (DishGpsStats sub-message at field 1015)
    bool    gps_valid;                                  // DishGpsStats[1]
    int     gps_sats;                                   // DishGpsStats[2]

    // -- Signal quality
    bool    is_snr_above_noise_floor;                   // field 1018
    bool    is_snr_persistently_low;                    // field 1022

    // -- Hardware / health
    int32_t eth_speed_mbps;                             // field 1016
    bool    stow_requested;                             // field 1010
    bool    is_cell_disabled;                           // field 1029
    bool    swupdate_reboot_ready;                      // field 1030
    bool    is_moving_fast_persisted;                   // field 1042

    // -- Nested stats
    StarlinkObstructionStats obstruction;               // field 1004
    StarlinkAlerts           alerts;                    // field 1005
    StarlinkReadyStates      ready_states;              // field 1019
};

// DishConfig (DishGetConfigResponse.dish_config[1])
struct StarlinkConfig {
    uint8_t  snow_melt_mode;                        // field 1 (SnowMeltMode enum)
    uint8_t  location_request_mode;                 // field 2 (LocationRequestMode enum)
    uint8_t  level_dish_mode;                       // field 3 (LevelDishMode enum)
    uint32_t power_save_start_minutes;              // field 4
    uint32_t power_save_duration_minutes;           // field 5
    bool     power_save_mode;                       // field 6
    bool     swupdate_three_day_deferral_enabled;   // field 7
    uint32_t swupdate_reboot_hour;                  // field 9
};

// ===============================================================================
// StarlinkClient
// ===============================================================================

class StarlinkClient {
public:
    StarlinkClient();

    // -- Data queries ----------------------------------------------------------
    bool get_location   ( StarlinkLocation*   out, uint32_t timeout_ms = 8000 );
    bool get_status     ( StarlinkStatus*     out, uint32_t timeout_ms = 8000 );
    bool get_device_info( StarlinkDeviceInfo* out, uint32_t timeout_ms = 8000 );
    bool get_time       ( uint64_t* unix_ns_out,   uint32_t timeout_ms = 8000 );
    bool get_config     ( StarlinkConfig*     out, uint32_t timeout_ms = 8000 );

    // -- Commands (returns true if the dish acknowledged) ----------------------
    bool reboot             ( uint32_t timeout_ms = 5000 );
    bool stow               ( uint32_t timeout_ms = 5000 );
    bool unstow             ( uint32_t timeout_ms = 5000 );
    bool set_inhibit_gps    ( bool inhibit, uint32_t timeout_ms = 5000 );
    bool enable_debug_telem ( uint32_t duration_minutes = 5,
                              uint32_t timeout_ms = 5000 );

private:
    enum class RpcId : uint8_t {
        GetLocation   = 0,
        GetStatus     = 1,
        GetDeviceInfo = 2,
        GetTime       = 3,
        GetConfig     = 4,
        Reboot        = 5,
        Stow          = 6,
        InhibitGps    = 7,
        DebugTelem    = 8,
    };

    bool run_rpc( RpcId rpc, uint32_t param, uint32_t timeout_ms );

    // lwIP callback statics
    static err_t s_connected( void* arg, struct tcp_pcb* pcb, err_t err );
    static err_t s_recv     ( void* arg, struct tcp_pcb* pcb,
                               struct pbuf* p, err_t err );
    static void  s_err      ( void* arg, err_t err );

    // Instance-level handlers
    err_t on_connected( struct tcp_pcb* pcb );
    err_t on_recv     ( struct tcp_pcb* pcb, struct pbuf* p );
    void  on_err      ();

    void send_preface_and_request( struct tcp_pcb* pcb );
    bool parse_response();

    // -- State -----------------------------------------------------------------
    struct tcp_pcb* m_pcb;
    void*           m_sem;          // SemaphoreHandle_t (opaque)
    RpcId           m_active_rpc;
    uint32_t        m_rpc_param;    // bool/uint32 parameter for command RPCs
    bool            m_success;

    static constexpr uint16_t RX_CAP = 1024;
    uint8_t  m_rx[ RX_CAP ];
    uint16_t m_rx_len;

    // Decoded result storage
    StarlinkLocation   m_location;
    StarlinkStatus     m_status;
    StarlinkDeviceInfo m_device_info;
    StarlinkConfig     m_config;
    uint64_t           m_server_time_ns;
};
