#include "starlink.hpp"
#include "shared.hpp"

#include "FreeRTOS.h"
#include "semphr.h"

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

#include <string.h>
#include <math.h>

// -- Starlink gRPC endpoint ----------------------------------------------------
#define STARLINK_IP    "192.168.100.1"
#define STARLINK_PORT  9200

// -- gRPC / Protobuf field numbers (verified from live protoset) ---------------
//
// Request message (SpaceX.API.Device.Request):
//   field 1001 = reboot           (empty sub-message)
//   field 1004 = get_status       (empty sub-message)
//   field 1008 = get_device_info  (empty sub-message)
//   field 1017 = get_location     (empty sub-message)
//   field 1034 = enable_debug_telem (EnableDebugTelemRequest { duration_m[1]: uint32 })
//   field 1037 = get_time         (empty sub-message)
//   field 2002 = stow             (DishStowRequest { unstow[1]: bool })
//   field 2011 = get_config       (empty sub-message)
//   field 2014 = inhibit_gps      (DishInhibitGpsRequest { inhibit_gps[1]: bool })
//
// Response message (SpaceX.API.Device.Response):
//   field 1004 = get_device_info  -> GetDeviceInfoResponse { device_info[1] -> DeviceInfo }
//   field 1017 = get_location     -> GetLocationResponse
//     field 1 = lla (LLAPosition) { lat[1], lon[2], alt[3] }
//     field 3 = source (PositionSource enum)
//     field 4 = sigma_m (double)
//     field 5 = horizontal_speed_mps (double)
//     field 6 = vertical_speed_mps   (double)
//   field 1037 = get_time         -> GetTimeResponse { unix_nano[1]: int64 }
//   field 2004 = dish_get_status  -> DishGetStatusResponse
//     field 1    = device_info    (DeviceInfo sub-message)
//     field 2    = device_state   (DeviceState { uptime_s[1]: uint64 })
//     field 1002 = seconds_to_first_nonempty_slot (float)
//     field 1003 = pop_ping_drop_rate             (float)
//     field 1004 = obstruction_stats (DishObstructionStats)
//       field 1  = fraction_obstructed (float)
//       field 4  = valid_s             (float)
//       field 5  = currently_obstructed (bool)
//       field 6  = avg_prolonged_obstruction_duration_s (float)
//       field 7  = avg_prolonged_obstruction_interval_s (float)
//       field 8  = avg_prolonged_obstruction_valid (bool)
//       field 9  = time_obstructed   (float)
//       field 10 = patches_valid     (uint32)
//     field 1005 = alerts (DishAlerts) — fields 1-11, 17-21 (bool each)
//     field 1007 = downlink_throughput_bps (float)
//     field 1008 = uplink_throughput_bps   (float)
//     field 1009 = pop_ping_latency_ms     (float)
//     field 1010 = stow_requested          (bool)
//     field 1011 = boresight_azimuth_deg   (float)
//     field 1012 = boresight_elevation_deg (float)
//     field 1015 = gps_stats (DishGpsStats { gps_valid[1], gps_sats[2] })
//     field 1016 = eth_speed_mbps          (int32)
//     field 1018 = is_snr_above_noise_floor (bool)
//     field 1019 = ready_states (DishReadyStates) — fields 1-6 (bool each)
//     field 1022 = is_snr_persistently_low  (bool)
//     field 1029 = is_cell_disabled         (bool)
//     field 1030 = swupdate_reboot_ready    (bool)
//     field 1042 = is_moving_fast_persisted (bool)
//   field 2011 = get_config -> DishGetConfigResponse { dish_config[1] -> DishConfig }
//     field 1 = snow_melt_mode                      (uint8 / enum)
//     field 2 = location_request_mode               (uint8 / enum)
//     field 3 = level_dish_mode                     (uint8 / enum)
//     field 4 = power_save_start_minutes            (uint32)
//     field 5 = power_save_duration_minutes         (uint32)
//     field 6 = power_save_mode                     (bool)
//     field 7 = swupdate_three_day_deferral_enabled (bool)
//     field 9 = swupdate_reboot_hour                (uint32)

// -- HTTP/2 frame type codes ---------------------------------------------------
#define H2_DATA       0x0
#define H2_HEADERS    0x1
#define H2_SETTINGS   0x4
#define H2_PING       0x6
#define H2_GOAWAY     0x7
#define H2_WUPDATE    0x8

// -- Protobuf wire types -------------------------------------------------------
#define WT_VARINT  0
#define WT_64BIT   1
#define WT_LEN     2
#define WT_32BIT   5

// -----------------------------------------------------------------------------
// Protobuf encoding helpers
// -----------------------------------------------------------------------------

static int pb_varint( uint8_t* buf, uint64_t v )
{
    int n = 0;
    do {
        buf[ n++ ] = ( v & 0x7F ) | ( v > 0x7F ? 0x80 : 0 );
        v >>= 7;
    } while ( v );
    return n;
}

static int pb_tag( uint8_t* buf, uint32_t field, uint8_t wire )
{
    return pb_varint( buf, ( ( uint64_t ) field << 3 ) | wire );
}

// -----------------------------------------------------------------------------
// Protobuf decoding helpers
// -----------------------------------------------------------------------------

static bool pb_decode_varint( const uint8_t* buf, uint16_t len,
                               uint16_t* pos, uint64_t* out )
{
    uint64_t val = 0;
    int shift = 0;
    while ( *pos < len ) {
        uint8_t b = buf[ (*pos)++ ];
        val |= ( uint64_t )( b & 0x7F ) << shift;
        shift += 7;
        if ( !( b & 0x80 ) ) { *out = val; return true; }
        if ( shift >= 64 ) break;
    }
    return false;
}

static bool pb_skip_field( const uint8_t* buf, uint16_t len,
                            uint16_t* pos, uint8_t wire )
{
    uint64_t v;
    switch ( wire ) {
        case WT_VARINT:
            return pb_decode_varint( buf, len, pos, &v );
        case WT_64BIT:
            if ( *pos + 8 > len ) return false;
            *pos += 8;
            return true;
        case WT_LEN:
            if ( !pb_decode_varint( buf, len, pos, &v ) ) return false;
            if ( *pos + v > len ) return false;
            *pos += ( uint16_t ) v;
            return true;
        case WT_32BIT:
            if ( *pos + 4 > len ) return false;
            *pos += 4;
            return true;
        default:
            return false;
    }
}

static double pb_read_double( const uint8_t* buf, uint16_t pos )
{
    double v;
    memcpy( &v, buf + pos, 8 );
    return v;
}

static float pb_read_float( const uint8_t* buf, uint16_t pos )
{
    float v;
    memcpy( &v, buf + pos, 4 );
    return v;
}

// Copy a length-delimited protobuf string field into a null-terminated buffer.
static void pb_copy_str( char* dst, size_t dst_size,
                          const uint8_t* src, uint16_t src_len )
{
    uint16_t n = ( src_len < ( uint16_t )( dst_size - 1 ) )
                 ? src_len : ( uint16_t )( dst_size - 1 );
    memcpy( dst, src, n );
    dst[ n ] = '\0';
}

// -----------------------------------------------------------------------------
// HTTP/2 frame builder
// -----------------------------------------------------------------------------

static uint16_t h2_frame( uint8_t* out,
                           uint8_t  type,
                           uint8_t  flags,
                           uint32_t stream_id,
                           const uint8_t* payload,
                           uint32_t payload_len )
{
    out[0] = ( payload_len >> 16 ) & 0xFF;
    out[1] = ( payload_len >>  8 ) & 0xFF;
    out[2] =   payload_len         & 0xFF;
    out[3] = type;
    out[4] = flags;
    out[5] = ( stream_id >> 24 ) & 0x7F;
    out[6] = ( stream_id >> 16 ) & 0xFF;
    out[7] = ( stream_id >>  8 ) & 0xFF;
    out[8] =   stream_id         & 0xFF;
    if ( payload && payload_len )
        memcpy( out + 9, payload, payload_len );
    return ( uint16_t )( 9 + payload_len );
}

// -----------------------------------------------------------------------------
// HPACK literal header encoding (no indexing, no Huffman)
// -----------------------------------------------------------------------------

static uint16_t hpack_literal( uint8_t* buf, const char* name, const char* val )
{
    uint16_t n = 0;
    buf[ n++ ] = 0x00;
    uint8_t nlen = ( uint8_t ) strlen( name );
    buf[ n++ ] = nlen;
    memcpy( buf + n, name, nlen ); n += nlen;
    uint8_t vlen = ( uint8_t ) strlen( val );
    buf[ n++ ] = vlen;
    memcpy( buf + n, val, vlen ); n += vlen;
    return n;
}

static uint16_t build_hpack_headers( uint8_t* buf, const char* grpc_method )
{
    uint16_t n = 0;
    n += hpack_literal( buf + n, ":method",       "POST" );
    n += hpack_literal( buf + n, ":scheme",       "http" );
    n += hpack_literal( buf + n, ":path",         grpc_method );
    n += hpack_literal( buf + n, ":authority",    STARLINK_IP ":" );
    n += hpack_literal( buf + n, "content-type",  "application/grpc" );
    n += hpack_literal( buf + n, "te",            "trailers" );
    return n;
}

// -----------------------------------------------------------------------------
// Protobuf request builders
// -----------------------------------------------------------------------------

// gRPC data frame: 5-byte header (compression=0, big-endian length) + protobuf
static uint16_t build_grpc_data( uint8_t* buf, const uint8_t* pb, uint16_t pb_len )
{
    buf[0] = 0;
    buf[1] = ( pb_len >> 24 ) & 0xFF;
    buf[2] = ( pb_len >> 16 ) & 0xFF;
    buf[3] = ( pb_len >>  8 ) & 0xFF;
    buf[4] =   pb_len         & 0xFF;
    memcpy( buf + 5, pb, pb_len );
    return ( uint16_t )( 5 + pb_len );
}

// Build: Request { [req_field] = { [sub_field]: sub_val } }
// If sub_field == 0, the sub-message is empty (no-argument RPCs and empty commands).
static uint16_t build_command_pb( uint8_t* buf,
                                   uint32_t req_field,
                                   uint32_t sub_field,
                                   uint32_t sub_val )
{
    uint16_t n = 0;
    if ( sub_field == 0 ) {
        // Empty sub-message
        n += ( uint16_t ) pb_tag( buf + n, req_field, WT_LEN );
        buf[ n++ ] = 0x00;
    } else {
        // Sub-message with one varint field
        uint8_t  inner[ 12 ];
        uint16_t inner_n = 0;
        inner_n += ( uint16_t ) pb_tag( inner + inner_n, sub_field, WT_VARINT );
        inner_n += ( uint16_t ) pb_varint( inner + inner_n, sub_val );

        n += ( uint16_t ) pb_tag( buf + n, req_field, WT_LEN );
        n += ( uint16_t ) pb_varint( buf + n, inner_n );
        memcpy( buf + n, inner, inner_n );
        n += inner_n;
    }
    return n;
}

// -----------------------------------------------------------------------------
// Response parsers — static helpers
// -----------------------------------------------------------------------------

// LLAPosition (lat[1], lon[2], alt[3]) — all 64-bit doubles
static bool parse_lla( const uint8_t* buf, uint16_t len, StarlinkLocation* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );
        if ( wire != WT_64BIT ) { pb_skip_field( buf, len, &pos, wire ); continue; }
        if ( pos + 8 > len ) return false;
        double v = pb_read_double( buf, pos ); pos += 8;
        if      ( field == 1 ) out->lat = v;
        else if ( field == 2 ) out->lon = v;
        else if ( field == 3 ) out->alt = v;
    }
    return true;
}

// GetLocationResponse (Response field 1017)
static bool parse_location_response( const uint8_t* buf, uint16_t len,
                                      StarlinkLocation* out )
{
    out->horizontal_speed_mps = 0.0;
    out->vertical_speed_mps   = 0.0;
    out->sigma_m              = 0.0;
    out->source               = PositionSource::AUTO;

    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return false;
            if ( pos + sub_len > len ) return false;

            if ( field == 1017 ) {
                // Walk GetLocationResponse
                const uint8_t* glr     = buf + pos;
                uint16_t       glr_len = ( uint16_t ) sub_len;
                uint16_t       glr_pos = 0;
                bool           got_lla = false;

                while ( glr_pos < glr_len ) {
                    uint64_t gtag;
                    if ( !pb_decode_varint( glr, glr_len, &glr_pos, &gtag ) ) break;
                    uint32_t gf = ( uint32_t )( gtag >> 3 );
                    uint8_t  gw = ( uint8_t  )( gtag & 7 );

                    if ( gf == 1 && gw == WT_LEN ) {
                        // LLAPosition sub-message
                        uint64_t llen;
                        if ( !pb_decode_varint( glr, glr_len, &glr_pos, &llen ) ) break;
                        if ( glr_pos + llen > glr_len ) break;
                        got_lla = parse_lla( glr + glr_pos, ( uint16_t ) llen, out );
                        glr_pos += ( uint16_t ) llen;
                    } else if ( gf == 3 && gw == WT_VARINT ) {
                        // source (PositionSource enum)
                        uint64_t v;
                        if ( pb_decode_varint( glr, glr_len, &glr_pos, &v ) )
                            out->source = ( PositionSource ) v;
                    } else if ( gw == WT_64BIT && glr_pos + 8 <= glr_len ) {
                        double v = pb_read_double( glr, glr_pos ); glr_pos += 8;
                        if      ( gf == 4 ) out->sigma_m              = v;
                        else if ( gf == 5 ) out->horizontal_speed_mps = v;
                        else if ( gf == 6 ) out->vertical_speed_mps   = v;
                    } else {
                        pb_skip_field( glr, glr_len, &glr_pos, gw );
                    }
                }
                return got_lla;
            }
            pos += ( uint16_t ) sub_len;
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
    return false;
}

// DeviceInfo (string fields + bools)
static void parse_device_info( const uint8_t* buf, uint16_t len,
                                StarlinkDeviceInfo* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return;
            if ( pos + sub_len > len ) return;
            switch ( field ) {
                case 1: pb_copy_str( out->id,               sizeof(out->id),               buf+pos, (uint16_t)sub_len ); break;
                case 2: pb_copy_str( out->hardware_version, sizeof(out->hardware_version), buf+pos, (uint16_t)sub_len ); break;
                case 3: pb_copy_str( out->software_version, sizeof(out->software_version), buf+pos, (uint16_t)sub_len ); break;
                case 4: pb_copy_str( out->country_code,     sizeof(out->country_code),     buf+pos, (uint16_t)sub_len ); break;
            }
            pos += ( uint16_t ) sub_len;
        } else if ( wire == WT_VARINT ) {
            uint64_t v;
            if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
            if      ( field == 7  ) out->is_dev  = ( v != 0 );
            else if ( field == 10 ) out->is_hitl = ( v != 0 );
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
}

// DishObstructionStats (DishGetStatusResponse field 1004)
static void parse_obstruction_stats( const uint8_t* buf, uint16_t len,
                                      StarlinkObstructionStats* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_32BIT ) {
            if ( pos + 4 > len ) return;
            float v = pb_read_float( buf, pos ); pos += 4;
            switch ( field ) {
                case 1: out->fraction_obstructed                  = v; break;
                case 4: out->valid_s                              = v; break;
                case 6: out->avg_prolonged_obstruction_duration_s = v; break;
                case 7: out->avg_prolonged_obstruction_interval_s = v; break;
                case 9: out->time_obstructed                      = v; break;
            }
        } else if ( wire == WT_VARINT ) {
            uint64_t v;
            if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
            switch ( field ) {
                case 5:  out->currently_obstructed             = ( v != 0 ); break;
                case 8:  out->avg_prolonged_obstruction_valid  = ( v != 0 ); break;
                case 10: out->patches_valid                    = ( uint32_t ) v; break;
            }
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
}

// DishAlerts (DishGetStatusResponse field 1005) — all bool/varint
static void parse_alerts( const uint8_t* buf, uint16_t len,
                           StarlinkAlerts* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );
        if ( wire != WT_VARINT ) { pb_skip_field( buf, len, &pos, wire ); continue; }
        uint64_t v;
        if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
        bool b = ( v != 0 );
        switch ( field ) {
            case 1:  out->motors_stuck                    = b; break;
            case 2:  out->thermal_shutdown                = b; break;
            case 3:  out->thermal_throttle                = b; break;
            case 4:  out->unexpected_location             = b; break;
            case 5:  out->mast_not_near_vertical          = b; break;
            case 6:  out->slow_ethernet_speeds            = b; break;
            case 7:  out->roaming                         = b; break;
            case 8:  out->install_pending                 = b; break;
            case 9:  out->is_heating                      = b; break;
            case 10: out->power_supply_thermal_throttle   = b; break;
            case 11: out->is_power_save_idle              = b; break;
            case 17: out->lower_signal_than_predicted     = b; break;
            case 18: out->slow_ethernet_speeds_100        = b; break;
            case 19: out->obstruction_map_reset           = b; break;
            case 20: out->dish_water_detected             = b; break;
            case 21: out->router_water_detected           = b; break;
        }
    }
}

// DishReadyStates (DishGetStatusResponse field 1019) — all bool/varint
static void parse_ready_states( const uint8_t* buf, uint16_t len,
                                 StarlinkReadyStates* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );
        if ( wire != WT_VARINT ) { pb_skip_field( buf, len, &pos, wire ); continue; }
        uint64_t v;
        if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
        bool b = ( v != 0 );
        switch ( field ) {
            case 1: out->cady = b; break;
            case 2: out->scp  = b; break;
            case 3: out->l1l2 = b; break;
            case 4: out->xphy = b; break;
            case 5: out->aap  = b; break;
            case 6: out->rf   = b; break;
        }
    }
}

// DishGpsStats (DishGetStatusResponse field 1015)
static void parse_gps_stats( const uint8_t* buf, uint16_t len,
                              StarlinkStatus* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );
        if ( wire != WT_VARINT ) { pb_skip_field( buf, len, &pos, wire ); continue; }
        uint64_t v;
        if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
        if      ( field == 1 ) out->gps_valid = ( v != 0 );
        else if ( field == 2 ) out->gps_sats  = ( int ) v;
    }
}

// DishGetStatusResponse (Response field 2004)
static void parse_dish_status( const uint8_t* buf, uint16_t len,
                                StarlinkStatus* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return;
            if ( pos + sub_len > len ) return;

            switch ( field ) {
                case 1:    // DeviceInfo
                    parse_device_info( buf+pos, (uint16_t)sub_len, &out->device_info );
                    break;
                case 2: {  // DeviceState { uptime_s[1]: uint64 }
                    const uint8_t* sb = buf + pos;
                    uint16_t sl = ( uint16_t ) sub_len, sp = 0;
                    while ( sp < sl ) {
                        uint64_t stag;
                        if ( !pb_decode_varint( sb, sl, &sp, &stag ) ) break;
                        uint32_t sf = ( uint32_t )( stag >> 3 );
                        uint8_t  sw = ( uint8_t  )( stag & 7 );
                        if ( sf == 1 && sw == WT_VARINT ) {
                            uint64_t v;
                            if ( pb_decode_varint( sb, sl, &sp, &v ) )
                                out->uptime_s = v;
                        } else {
                            pb_skip_field( sb, sl, &sp, sw );
                        }
                    }
                    break;
                }
                case 1004: parse_obstruction_stats( buf+pos, (uint16_t)sub_len, &out->obstruction   ); break;
                case 1005: parse_alerts            ( buf+pos, (uint16_t)sub_len, &out->alerts        ); break;
                case 1015: parse_gps_stats         ( buf+pos, (uint16_t)sub_len, out                 ); break;
                case 1019: parse_ready_states      ( buf+pos, (uint16_t)sub_len, &out->ready_states  ); break;
            }
            pos += ( uint16_t ) sub_len;

        } else if ( wire == WT_32BIT ) {
            if ( pos + 4 > len ) return;
            float v = pb_read_float( buf, pos ); pos += 4;
            switch ( field ) {
                case 1002: out->seconds_to_first_nonempty_slot = v; break;
                case 1003: out->pop_ping_drop_rate             = v; break;
                case 1007: out->downlink_throughput_bps        = v; break;
                case 1008: out->uplink_throughput_bps          = v; break;
                case 1009: out->pop_ping_latency_ms            = v; break;
                case 1011: out->boresight_azimuth_deg          = v; break;
                case 1012: out->boresight_elevation_deg        = v; break;
            }
        } else if ( wire == WT_VARINT ) {
            uint64_t v;
            if ( !pb_decode_varint( buf, len, &pos, &v ) ) return;
            switch ( field ) {
                case 1010: out->stow_requested           = ( v != 0 ); break;
                case 1016: out->eth_speed_mbps           = ( int32_t ) v; break;
                case 1018: out->is_snr_above_noise_floor = ( v != 0 ); break;
                case 1022: out->is_snr_persistently_low  = ( v != 0 ); break;
                case 1029: out->is_cell_disabled         = ( v != 0 ); break;
                case 1030: out->swupdate_reboot_ready    = ( v != 0 ); break;
                case 1042: out->is_moving_fast_persisted = ( v != 0 ); break;
            }
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
}

static bool parse_status_response( const uint8_t* buf, uint16_t len,
                                    StarlinkStatus* out )
{
    memset( out, 0, sizeof(*out) );

    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return false;
            if ( pos + sub_len > len ) return false;
            if ( field == 2004 ) {
                parse_dish_status( buf + pos, ( uint16_t ) sub_len, out );
                return true;
            }
            pos += ( uint16_t ) sub_len;
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
    return false;
}

// GetTimeResponse (Response field 1037) — { unix_nano[1]: int64 }
static bool parse_time_response( const uint8_t* buf, uint16_t len,
                                  uint64_t* out )
{
    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return false;
            if ( pos + sub_len > len ) return false;

            if ( field == 1037 ) {
                // GetTimeResponse { unix_nano[1]: int64 (varint) }
                const uint8_t* tr = buf + pos;
                uint16_t tr_len = ( uint16_t ) sub_len, tp = 0;
                while ( tp < tr_len ) {
                    uint64_t ttag;
                    if ( !pb_decode_varint( tr, tr_len, &tp, &ttag ) ) break;
                    uint32_t tf = ( uint32_t )( ttag >> 3 );
                    uint8_t  tw = ( uint8_t  )( ttag & 7 );
                    if ( tf == 1 && tw == WT_VARINT ) {
                        uint64_t v;
                        if ( pb_decode_varint( tr, tr_len, &tp, &v ) ) {
                            *out = v;
                            return true;
                        }
                    }
                    pb_skip_field( tr, tr_len, &tp, tw );
                }
            }
            pos += ( uint16_t ) sub_len;
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
    return false;
}

// GetDeviceInfoResponse (Response field 1004) — { device_info[1] -> DeviceInfo }
static bool parse_device_info_response( const uint8_t* buf, uint16_t len,
                                         StarlinkDeviceInfo* out )
{
    memset( out, 0, sizeof(*out) );

    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return false;
            if ( pos + sub_len > len ) return false;

            if ( field == 1004 ) {
                // GetDeviceInfoResponse { device_info[1] -> DeviceInfo }
                const uint8_t* dr = buf + pos;
                uint16_t dr_len = ( uint16_t ) sub_len, dp = 0;
                while ( dp < dr_len ) {
                    uint64_t dtag;
                    if ( !pb_decode_varint( dr, dr_len, &dp, &dtag ) ) break;
                    uint32_t df = ( uint32_t )( dtag >> 3 );
                    uint8_t  dw = ( uint8_t  )( dtag & 7 );
                    if ( df == 1 && dw == WT_LEN ) {
                        uint64_t dlen;
                        if ( !pb_decode_varint( dr, dr_len, &dp, &dlen ) ) break;
                        if ( dp + dlen <= dr_len ) {
                            parse_device_info( dr + dp, ( uint16_t ) dlen, out );
                            return true;
                        }
                    }
                    pb_skip_field( dr, dr_len, &dp, dw );
                }
            }
            pos += ( uint16_t ) sub_len;
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
    return false;
}

// DishGetConfigResponse (Response field 2011) — { dish_config[1] -> DishConfig }
static bool parse_config_response( const uint8_t* buf, uint16_t len,
                                    StarlinkConfig* out )
{
    memset( out, 0, sizeof(*out) );

    uint16_t pos = 0;
    while ( pos < len ) {
        uint64_t tag_v;
        if ( !pb_decode_varint( buf, len, &pos, &tag_v ) ) return false;
        uint32_t field = ( uint32_t )( tag_v >> 3 );
        uint8_t  wire  = ( uint8_t  )( tag_v & 7 );

        if ( wire == WT_LEN ) {
            uint64_t sub_len;
            if ( !pb_decode_varint( buf, len, &pos, &sub_len ) ) return false;
            if ( pos + sub_len > len ) return false;

            if ( field == 2011 ) {
                // DishGetConfigResponse { dish_config[1] -> DishConfig }
                const uint8_t* cr = buf + pos;
                uint16_t cr_len = ( uint16_t ) sub_len, cp = 0;
                while ( cp < cr_len ) {
                    uint64_t ctag;
                    if ( !pb_decode_varint( cr, cr_len, &cp, &ctag ) ) break;
                    uint32_t cf = ( uint32_t )( ctag >> 3 );
                    uint8_t  cw = ( uint8_t  )( ctag & 7 );
                    if ( cf == 1 && cw == WT_LEN ) {
                        uint64_t clen;
                        if ( !pb_decode_varint( cr, cr_len, &cp, &clen ) ) break;
                        if ( cp + clen > cr_len ) break;
                        // DishConfig sub-message
                        const uint8_t* dc = cr + cp;
                        uint16_t dc_len = ( uint16_t ) clen, dp = 0;
                        while ( dp < dc_len ) {
                            uint64_t dtag;
                            if ( !pb_decode_varint( dc, dc_len, &dp, &dtag ) ) break;
                            uint32_t df = ( uint32_t )( dtag >> 3 );
                            uint8_t  dw = ( uint8_t  )( dtag & 7 );
                            if ( dw == WT_VARINT ) {
                                uint64_t v;
                                if ( !pb_decode_varint( dc, dc_len, &dp, &v ) ) break;
                                switch ( df ) {
                                    case 1: out->snow_melt_mode                      = ( uint8_t  ) v; break;
                                    case 2: out->location_request_mode               = ( uint8_t  ) v; break;
                                    case 3: out->level_dish_mode                     = ( uint8_t  ) v; break;
                                    case 4: out->power_save_start_minutes            = ( uint32_t ) v; break;
                                    case 5: out->power_save_duration_minutes         = ( uint32_t ) v; break;
                                    case 6: out->power_save_mode                     = ( v != 0 );     break;
                                    case 7: out->swupdate_three_day_deferral_enabled = ( v != 0 );     break;
                                    case 9: out->swupdate_reboot_hour                = ( uint32_t ) v; break;
                                }
                            } else {
                                pb_skip_field( dc, dc_len, &dp, dw );
                            }
                        }
                        cp += ( uint16_t ) clen;
                        return true;
                    }
                    pb_skip_field( cr, cr_len, &cp, cw );
                }
            }
            pos += ( uint16_t ) sub_len;
        } else {
            pb_skip_field( buf, len, &pos, wire );
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// StarlinkClient implementation
// -----------------------------------------------------------------------------

static StaticSemaphore_t s_sem_buf;

StarlinkClient::StarlinkClient()
    : m_pcb( nullptr )
    , m_sem( nullptr )
    , m_active_rpc( RpcId::GetLocation )
    , m_rpc_param( 0 )
    , m_success( false )
    , m_rx_len( 0 )
    , m_server_time_ns( 0 )
{
    m_sem = xSemaphoreCreateBinaryStatic( &s_sem_buf );
    memset( &m_location,    0, sizeof(m_location) );
    memset( &m_status,      0, sizeof(m_status) );
    memset( &m_device_info, 0, sizeof(m_device_info) );
    memset( &m_config,      0, sizeof(m_config) );
}

// -- lwIP callbacks ------------------------------------------------------------

err_t StarlinkClient::s_connected( void* arg, struct tcp_pcb* pcb, err_t err )
{
    ( void ) err;
    return static_cast<StarlinkClient*>(arg)->on_connected( pcb );
}

err_t StarlinkClient::s_recv( void* arg, struct tcp_pcb* pcb,
                               struct pbuf* p, err_t err )
{
    ( void ) err;
    return static_cast<StarlinkClient*>(arg)->on_recv( pcb, p );
}

void StarlinkClient::s_err( void* arg, err_t err )
{
    ( void ) err;
    static_cast<StarlinkClient*>(arg)->on_err();
}

err_t StarlinkClient::on_connected( struct tcp_pcb* pcb )
{
    tcp_recv( pcb, s_recv );
    send_preface_and_request( pcb );
    return ERR_OK;
}

// Reassemble incoming HTTP/2 DATA frames and collect the gRPC protobuf payload.
err_t StarlinkClient::on_recv( struct tcp_pcb* pcb, struct pbuf* p )
{
    if ( !p ) {
        // Server closed connection — parse accumulated buffer
        m_success = parse_response();
        tcp_arg( pcb, nullptr );
        tcp_close( pcb );
        m_pcb = nullptr;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR( ( SemaphoreHandle_t ) m_sem, &woken );
        portYIELD_FROM_ISR( woken );
        return ERR_OK;
    }

    struct pbuf* q = p;
    while ( q ) {
        const uint8_t* src     = ( const uint8_t* ) q->payload;
        uint16_t       src_len = q->len;
        uint16_t       i       = 0;

        // Scan for HTTP/2 frames (9-byte headers)
        while ( i + 9 <= src_len ) {
            uint32_t frame_len = ( ( uint32_t ) src[i]   << 16 )
                               | ( ( uint32_t ) src[i+1] <<  8 )
                               |                src[i+2];
            uint8_t  frame_type = src[i+3];
            i += 9;

            if ( i + frame_len > src_len ) break;   // partial frame — stop

            if ( frame_type == H2_DATA && frame_len > 5 ) {
                // Skip the 5-byte gRPC message header
                const uint8_t* payload = src + i + 5;
                uint16_t       pay_len = ( uint16_t )( frame_len - 5 );
                if ( m_rx_len + pay_len <= RX_CAP ) {
                    memcpy( m_rx + m_rx_len, payload, pay_len );
                    m_rx_len += pay_len;
                }
            }

            i += ( uint16_t ) frame_len;
        }
        q = q->next;
    }

    tcp_recved( pcb, p->tot_len );
    pbuf_free( p );
    return ERR_OK;
}

void StarlinkClient::on_err()
{
    m_pcb     = nullptr;
    m_success = false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR( ( SemaphoreHandle_t ) m_sem, &woken );
    portYIELD_FROM_ISR( woken );
}

// -- Build and transmit the HTTP/2 + gRPC request ------------------------------

void StarlinkClient::send_preface_and_request( struct tcp_pcb* pcb )
{
    static constexpr char k_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    uint8_t  buf[ 512 ];
    uint16_t n = 0;

    // 1. Client connection preface (24 bytes)
    memcpy( buf + n, k_preface, 24 ); n += 24;

    // 2. Empty SETTINGS frame
    static const uint8_t k_settings[9] = { 0,0,0, H2_SETTINGS, 0, 0,0,0,0 };
    memcpy( buf + n, k_settings, 9 ); n += 9;

    // 3. HEADERS frame (stream 1, END_HEADERS)
    uint8_t  hpack[ 256 ];
    uint16_t hpack_len = build_hpack_headers( hpack, "/SpaceX.API.Device.Device/Handle" );
    n += h2_frame( buf + n, H2_HEADERS, 0x04, 1, hpack, hpack_len );

    // 4. Protobuf body — map RpcId to (req_field, sub_field, sub_val)
    uint32_t req_field, sub_field = 0, sub_val = 0;
    switch ( m_active_rpc ) {
        case RpcId::GetLocation:   req_field = 1017; break;
        case RpcId::GetStatus:     req_field = 1004; break;
        case RpcId::GetDeviceInfo: req_field = 1008; break;
        case RpcId::GetTime:       req_field = 1037; break;
        case RpcId::GetConfig:     req_field = 2011; break;
        case RpcId::Reboot:        req_field = 1001; break;
        case RpcId::Stow:          req_field = 2002; sub_field = 1; sub_val = m_rpc_param; break;
        case RpcId::InhibitGps:    req_field = 2014; sub_field = 1; sub_val = m_rpc_param; break;
        case RpcId::DebugTelem:    req_field = 1034; sub_field = 1; sub_val = m_rpc_param; break;
        default:                   req_field = 1004; break;
    }

    uint8_t  pb[ 32 ];
    uint16_t pb_len = build_command_pb( pb, req_field, sub_field, sub_val );

    uint8_t  grpc_data[ 37 ];   // 5 + 32
    uint16_t grpc_len = build_grpc_data( grpc_data, pb, pb_len );

    // 5. DATA frame (stream 1, END_STREAM)
    n += h2_frame( buf + n, H2_DATA, 0x01, 1, grpc_data, grpc_len );

    tcp_write( pcb, buf, n, TCP_WRITE_FLAG_COPY );
    tcp_output( pcb );
}

// -- Dispatch accumulated rx buffer to the right parser ------------------------

bool StarlinkClient::parse_response()
{
    if ( m_rx_len == 0 && m_active_rpc >= RpcId::Reboot ) return true; // commands ok on empty

    switch ( m_active_rpc ) {
        case RpcId::GetLocation:   return parse_location_response    ( m_rx, m_rx_len, &m_location    );
        case RpcId::GetStatus:     return parse_status_response      ( m_rx, m_rx_len, &m_status      );
        case RpcId::GetDeviceInfo: return parse_device_info_response ( m_rx, m_rx_len, &m_device_info );
        case RpcId::GetTime:       return parse_time_response        ( m_rx, m_rx_len, &m_server_time_ns );
        case RpcId::GetConfig:     return parse_config_response      ( m_rx, m_rx_len, &m_config      );
        default:                   return true;   // command RPCs — server closed = success
    }
}

// -- Core RPC executor ---------------------------------------------------------

bool StarlinkClient::run_rpc( RpcId rpc, uint32_t param, uint32_t timeout_ms )
{
    m_active_rpc = rpc;
    m_rpc_param  = param;
    m_success    = false;
    m_rx_len     = 0;

    ip_addr_t ip;
    ipaddr_aton( STARLINK_IP, &ip );

    cyw43_arch_lwip_begin();
    m_pcb = tcp_new_ip_type( IPADDR_TYPE_V4 );
    err_t err = ERR_MEM;
    if ( m_pcb ) {
        tcp_arg( m_pcb, this );
        tcp_err( m_pcb, s_err );
        err = tcp_connect( m_pcb, &ip, STARLINK_PORT, s_connected );
    }
    cyw43_arch_lwip_end();

    if ( err != ERR_OK ) {
        log_print( "[starlink] tcp_connect error %d\n", err );
        return false;
    }

    if ( xSemaphoreTake( ( SemaphoreHandle_t ) m_sem,
                          pdMS_TO_TICKS( timeout_ms ) ) != pdTRUE ) {
        log_print( "[starlink] RPC timed out\n" );
        cyw43_arch_lwip_begin();
        if ( m_pcb ) { tcp_abort( m_pcb ); m_pcb = nullptr; }
        cyw43_arch_lwip_end();
        return false;
    }

    return m_success;
}

// -- Public methods -------------------------------------------------------------

bool StarlinkClient::get_location( StarlinkLocation* out, uint32_t timeout_ms )
{
    if ( !run_rpc( RpcId::GetLocation, 0, timeout_ms ) ) return false;
    *out = m_location;
    return true;
}

bool StarlinkClient::get_status( StarlinkStatus* out, uint32_t timeout_ms )
{
    if ( !run_rpc( RpcId::GetStatus, 0, timeout_ms ) ) return false;
    *out = m_status;
    return true;
}

bool StarlinkClient::get_device_info( StarlinkDeviceInfo* out, uint32_t timeout_ms )
{
    if ( !run_rpc( RpcId::GetDeviceInfo, 0, timeout_ms ) ) return false;
    *out = m_device_info;
    return true;
}

bool StarlinkClient::get_time( uint64_t* unix_ns_out, uint32_t timeout_ms )
{
    if ( !run_rpc( RpcId::GetTime, 0, timeout_ms ) ) return false;
    *unix_ns_out = m_server_time_ns;
    return true;
}

bool StarlinkClient::get_config( StarlinkConfig* out, uint32_t timeout_ms )
{
    if ( !run_rpc( RpcId::GetConfig, 0, timeout_ms ) ) return false;
    *out = m_config;
    return true;
}

bool StarlinkClient::reboot( uint32_t timeout_ms )
{
    return run_rpc( RpcId::Reboot, 0, timeout_ms );
}

bool StarlinkClient::stow( uint32_t timeout_ms )
{
    return run_rpc( RpcId::Stow, 0 /*unstow=false*/, timeout_ms );
}

bool StarlinkClient::unstow( uint32_t timeout_ms )
{
    return run_rpc( RpcId::Stow, 1 /*unstow=true*/, timeout_ms );
}

bool StarlinkClient::set_inhibit_gps( bool inhibit, uint32_t timeout_ms )
{
    return run_rpc( RpcId::InhibitGps, inhibit ? 1u : 0u, timeout_ms );
}

bool StarlinkClient::enable_debug_telem( uint32_t duration_minutes,
                                          uint32_t timeout_ms )
{
    return run_rpc( RpcId::DebugTelem, duration_minutes, timeout_ms );
}
