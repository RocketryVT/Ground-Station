#pragma once

#include "shared.hpp"

#include "Radio.hpp"
#include "SIGMA.hpp"
#include "SIGMA2/SIGMA2.hpp"
#include "SIGMA2/packets/gps_packets.hpp"
#include "SIGMA2/packets/mesh_packets.hpp"

#include <math.h>

namespace lora_bridge {

static inline SIGMA::FlightState map_sigma2_state(SIGMA2::FLIGHT_STATE state)
{
    switch (state) {
    case SIGMA2::FLIGHT_STATE::PAD:
    case SIGMA2::FLIGHT_STATE::UNKOWN:
        return SIGMA::FlightState::GROUND_IDLE;
    case SIGMA2::FLIGHT_STATE::BOOST:
        return SIGMA::FlightState::POWERED_ASCENT;
    case SIGMA2::FLIGHT_STATE::COAST:
        return SIGMA::FlightState::COAST_ASCENT;
    case SIGMA2::FLIGHT_STATE::APOGEE:
        return SIGMA::FlightState::APOGEE;
    case SIGMA2::FLIGHT_STATE::DESCENT:
        return SIGMA::FlightState::DESCENT_DROGUE;
    case SIGMA2::FLIGHT_STATE::LANDED:
        return SIGMA::FlightState::LANDED;
    default:
        return SIGMA::FlightState::FAULT;
    }
}

static inline float speed_from_cms(int16_t vn_cms, int16_t ve_cms, int16_t vd_cms)
{
    const float vn = static_cast<float>(vn_cms) * 0.01f;
    const float ve = static_cast<float>(ve_cms) * 0.01f;
    const float vd = static_cast<float>(vd_cms) * 0.01f;
    return sqrtf((vn * vn) + (ve * ve) + (vd * vd));
}

static inline float q15_to_float(int16_t q)
{
    return static_cast<float>(q) / 32768.0f;
}

static inline bool enqueue_inter_pico(const SIGMA::InterPicoData& data)
{
    UdpFrame frame;
    frame.len = static_cast<uint16_t>(data.serialize(frame.data, sizeof(frame.data)));
    if (frame.len == 0u) {
        return false;
    }
    return xQueueSend(g_udp_queue, &frame, 0) == pdTRUE;
}

static inline bool handle_sigma2_gps_nav(const radio::Packet& pkt,
                                          const char* tag,
                                          const SIGMA2::DecodedFrame& frame)
{
    SIGMA2::TRANSMIT_PACKETS::GPSNav gps = {};
    if (!SIGMA2::deserialize_packet_payload(frame, gps)) {
        return false;
    }

    SIGMA::InterPicoData out;
    out.boot_ms = frame.header.timestamp_ms;
    out.lat = static_cast<double>(gps.lat_deg_e7) * 1.0e-7;
    out.lon = static_cast<double>(gps.lon_deg_e7) * 1.0e-7;
    out.alt_gps_m = static_cast<float>(gps.alt_msl_cm) * 0.01f;
    out.alt_baro_m = out.alt_gps_m;
    out.speed_ms = speed_from_cms(gps.vel_n_cms, gps.vel_e_cms, gps.vel_d_cms);
    out.vel_ned_ms[0] = static_cast<float>(gps.vel_n_cms) * 0.01f;
    out.vel_ned_ms[1] = static_cast<float>(gps.vel_e_cms) * 0.01f;
    out.vel_ned_ms[2] = static_cast<float>(gps.vel_d_cms) * 0.01f;
    out.h_acc_m = static_cast<float>(gps.h_acc_cm) * 0.01f;
    out.v_acc_m = static_cast<float>(gps.v_acc_cm) * 0.01f;
    out.s_acc_ms = static_cast<float>(gps.s_acc_cms) * 0.01f;
    out.state = SIGMA::FlightState::GROUND_IDLE;
    out.satellites = gps.num_sv;
    out.flags = gps.flags;
    out.rssi = static_cast<int>(pkt.rssi);
    out.snr_dB = pkt.snr;
    const bool enqueued = enqueue_inter_pico(out);
    log_print("ALT,%.2f,%lu,%.1f,%.1f\n",
              static_cast<double>(out.alt_baro_m),
              static_cast<unsigned long>(out.boot_ms),
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr));
    log_print("[%s] SIGMA2 GPS lat=%.5f lon=%.5f alt=%.0f m sats=%u "
              "vel=%.1f m/s hacc=%.1f m RSSI=%.0f SNR=%.1f %s\n",
              tag,
              out.lat,
              out.lon,
              static_cast<double>(out.alt_gps_m),
              static_cast<unsigned>(out.satellites),
              static_cast<double>(out.speed_ms),
              static_cast<double>(out.h_acc_m),
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr),
              enqueued ? "queued" : "udp queue full");
    return true;
}

static inline bool handle_sigma2_nav_state(const radio::Packet& pkt,
                                           const char* tag,
                                           const SIGMA2::DecodedFrame& frame)
{
    SIGMA2::TRANSMIT_PACKETS::NavState nav = {};
    if (!SIGMA2::deserialize_packet_payload(frame, nav)) {
        return false;
    }

    SIGMA::InterPicoData out;
    out.boot_ms = frame.header.timestamp_ms;
    out.lat = static_cast<double>(nav.lat_deg_e7) * 1.0e-7;
    out.lon = static_cast<double>(nav.lon_deg_e7) * 1.0e-7;
    out.alt_gps_m = static_cast<float>(nav.alt_fused_cm) * 0.01f;
    out.alt_baro_m = out.alt_gps_m;
    out.speed_ms = speed_from_cms(nav.vel_n_cms, nav.vel_e_cms, nav.vel_d_cms);
    out.vel_ned_ms[0] = static_cast<float>(nav.vel_n_cms) * 0.01f;
    out.vel_ned_ms[1] = static_cast<float>(nav.vel_e_cms) * 0.01f;
    out.vel_ned_ms[2] = static_cast<float>(nav.vel_d_cms) * 0.01f;
    out.acc_ned_ms2[0] = static_cast<float>(nav.acc_n_mg) * 0.00980665f;
    out.acc_ned_ms2[1] = static_cast<float>(nav.acc_e_mg) * 0.00980665f;
    out.acc_ned_ms2[2] = static_cast<float>(nav.acc_d_mg) * 0.00980665f;
    out.nav_source = nav.nav_source;
    out.q[0] = q15_to_float(nav.q[0]);
    out.q[1] = q15_to_float(nav.q[1]);
    out.q[2] = q15_to_float(nav.q[2]);
    out.q[3] = q15_to_float(nav.q[3]);
    out.state = map_sigma2_state(nav.state);
    out.flags = nav.flags;
    out.rssi = static_cast<int>(pkt.rssi);
    out.snr_dB = pkt.snr;
    const bool enqueued = enqueue_inter_pico(out);
    log_print("ALT,%.2f,%lu,%.1f,%.1f\n",
              static_cast<double>(out.alt_baro_m),
              static_cast<unsigned long>(out.boot_ms),
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr));
    log_print("[%s] SIGMA2 NAV lat=%.5f lon=%.5f alt=%.0f m "
              "vn=%.1f ve=%.1f vd=%.1f RSSI=%.0f SNR=%.1f %s\n",
              tag,
              out.lat,
              out.lon,
              static_cast<double>(out.alt_gps_m),
              static_cast<double>(out.vel_ned_ms[0]),
              static_cast<double>(out.vel_ned_ms[1]),
              static_cast<double>(out.vel_ned_ms[2]),
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr),
              enqueued ? "queued" : "udp queue full");
    return true;
}

static inline bool handle_sigma2_baro(const radio::Packet& pkt,
                                      const char* tag,
                                      const SIGMA2::DecodedFrame& frame)
{
    SIGMA2::TRANSMIT_PACKETS::Barometer baro = {};
    if (!SIGMA2::deserialize_packet_payload(frame, baro)) {
        return false;
    }

    const float alt_m = static_cast<float>(baro.altitude_cm) * 0.01f;
    log_print("ALT,%.2f,%lu,%.1f,%.1f\n",
              static_cast<double>(alt_m),
              static_cast<unsigned long>(frame.header.timestamp_ms),
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr));
    log_print("[%s] SIGMA2 BARO alt=%.1f m  P=%ld Pa  T=%.2f C  RSSI=%.0f SNR=%.1f\n",
              tag,
              static_cast<double>(alt_m),
              static_cast<long>(baro.pressure_pa),
              static_cast<double>(baro.temperature_cc) * 0.01,
              static_cast<double>(pkt.rssi),
              static_cast<double>(pkt.snr));
    return true;
}

static inline bool handle_sigma2_packet(const radio::Packet& pkt, const char* tag)
{
    SIGMA2::DecodedFrame frame = {};
    const SIGMA2::DecodeStatus status =
        SIGMA2::deserialize_frame(pkt.data, pkt.len, frame);
    if (status != SIGMA2::DecodeStatus::Ok) {
        return false;
    }

    switch (frame.header.type) {
    case SIGMA2::PacketType::GPS:
        return handle_sigma2_gps_nav(pkt, tag, frame);
    case SIGMA2::PacketType::NAV_STATE:
        return handle_sigma2_nav_state(pkt, tag, frame);
    case SIGMA2::PacketType::BARO:
        return handle_sigma2_baro(pkt, tag, frame);
    case SIGMA2::PacketType::TIMESYNC:
        return true;
    default:
        log_print("[%s] SIGMA2 frame type %u ignored (%u B RSSI %.0f SNR %.1f)\n",
                  tag,
                  static_cast<unsigned>(frame.header.type),
                  static_cast<unsigned>(pkt.len),
                  static_cast<double>(pkt.rssi),
                  static_cast<double>(pkt.snr));
        return true;
    }
}

} // namespace lora_bridge
