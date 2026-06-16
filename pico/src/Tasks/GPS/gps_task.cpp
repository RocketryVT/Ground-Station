#include "gps_task.hpp"
#include "shared.hpp"
#include "Tasks/RTC/rtc.hpp"

// Pico SDK UART header before gps_driver.hpp (see gps_driver.hpp transport section).
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "gps/gps_driver.hpp"

#include <cstddef>
#include <math.h>
#include <new>

// ── Averaging parameters ──────────────────────────────────────────────────────
// Collect a running Welford average until the estimate stabilises, then freeze
// and do periodic drift checks.  Same tuning as the old Starlink path.

#define POLL_INTERVAL_MS         10
#define POLL_INTERVAL_CONVERGED_MS 60000

#define MIN_SAMPLES      12
#define MAX_SAMPLES      120
#define CONVERGE_SIGMA_M 1.5

using Driver = gps::PicoGpsDriver;

alignas( Driver ) static uint8_t s_driver_buf[ sizeof( Driver ) ];
static Driver* s_driver = nullptr;

static constexpr Board::GpsInstance GPS = Board::Gpses[0];
static constexpr uint16_t GPS_RATE_MS = static_cast<uint16_t>( 1000u / GPS.nav_hz );

// ── Welford online mean/variance ──────────────────────────────────────────────

struct Welford {
    double w_sum = 0.0;
    double mean  = 0.0;
    double M2    = 0.0;
    int    n     = 0;

    void update(double x, double w) {
        w_sum    += w;
        double old = mean;
        mean     += (w / w_sum) * (x - old);
        M2       += w * (x - old) * (x - mean);
        ++n;
    }

    double stddev() const {
        if (n < 2 || w_sum == 0.0) return 1e9;
        return sqrt(M2 / w_sum);
    }
};

static void gps_task( void* )
{
    s_driver = new( s_driver_buf ) Driver( gps::PicoGpsConfig{
        .uart = uart0,
        .tx_pin = Pins::GPS_TX,
        .rx_pin = Pins::GPS_RX,
        .desired_baud = GPS.baud,
        .rx_mode = gps::UartRxMode::DmaRing,
        .dma_ring_size = gps::DmaRingBufferSize::Bytes2K,
    } );

    if ( !s_driver->rx_mode_ok() ) {
        log_print( "[gps] DMA ring unavailable; falling back to polling RX\n" );
    }

    if ( !s_driver->initialized() ) {
        log_print( "[gps] autobaud failed; no checksum-valid GPS UART data detected\n" );
    } else {
        log_print( "[gps] autobaud detected %lu baud; target %lu baud %s\n",
                   (unsigned long)s_driver->detected_baud(),
                   (unsigned long)s_driver->desired_baud(),
                   s_driver->baud_change_ok() ? "ok" : "failed" );
    }

    auto drain = [&]( uint32_t ms ) {
        const uint32_t slices = ms / 10u;
        for ( uint32_t i = 0; i < slices; ++i ) {
            uint8_t tmp[ 256 ];
            const std::size_t n = s_driver->read_raw( tmp, sizeof tmp );
            s_driver->feed_ubx_only( tmp, n );
            vTaskDelay( pdMS_TO_TICKS( 10 ) );
        }
    };

    auto send = [&]( const gps::UbxFrame& f, uint32_t ms = 100 ) {
        s_driver->send_ubx( f );
        drain( ms );
    };

    // Configure M10/M9-style receivers via CFG-VALSET in RAM. The driver has
    // already detected the live module baud and switched to the profile target.
    send( gps::Ubx::valset_uart1_inprot_ubx( true, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_uart1_inprot_nmea( true, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_uart1_outprot_ubx( true, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_uart1_outprot_nmea( false, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_rate_meas( GPS_RATE_MS, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_rate_nav( 1, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_nav_pvt_uart1( 1, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_fix_mode( gps::Ubx::FixMode::Auto, gps::ValLayer::RAM ) );
    send( gps::Ubx::valset_dyn_model( gps::Ubx::DynModel::Stationary,
                                      gps::ValLayer::RAM ), 500 );

    log_print( "[gps] %s init done - UART0 TX=GPIO%u RX=GPIO%u, %lu baud, %u Hz, UBX NAV-PVT\n",
               Board::spec_of( GPS.model ).name,
               Pins::GPS_TX, Pins::GPS_RX,
               (unsigned long)s_driver->current_baud(),
               (unsigned)GPS.nav_hz );
    log_print( "[gps] RX mode=%s dma_ch=%d ring=%luB\n",
               s_driver->rx_mode() == gps::UartRxMode::DmaRing ? "dma" : "poll",
               s_driver->dma_channel(),
               (unsigned long)s_driver->dma_ring_size_bytes() );

    Welford lat_w, lon_w, alt_w;
    bool     converged     = false;
    uint32_t last_pvt      = 0;
    uint64_t last_check_us = 0;

    for ( ;; ) {
        uint8_t rx_buf[ 256 ];
        const std::size_t n = s_driver->read_raw( rx_buf, sizeof rx_buf );
        s_driver->feed_ubx_only( rx_buf, n );

        const uint32_t pvt_count = s_driver->diagnostics().ubx_pvt;
        if ( pvt_count == last_pvt ) {
            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_MS ) );
            continue;
        }
        last_pvt = pvt_count;

        if ( !s_driver->has_fix() ) {
            log_print( "[gps] waiting for fix (%s)\n",
                       s_driver->fix_label().data() );
            continue;
        }

        const auto&  c       = s_driver->coordinate();

        // Discipline the software RTC on every resolved fix.  The RTC service
        // handles its own warm-up + 60 s resync gating, so feed unconditionally
        // (this also runs after position convergence, where the loop below may
        // continue early).
        rtc_submit_gps_time( c.utc_year, c.utc_month, c.utc_day,
                             c.utc_ms, time_us_64() );

        const double sigma_m = c.h_acc_mm / 1000.0;

        if ( sigma_m <= 0.0 ) continue;

        if ( !converged ) {
            double w = 1.0 / (sigma_m * sigma_m);
            lat_w.update( c.latitude,  w );
            lon_w.update( c.longitude, w );
            alt_w.update( c.altitude,  w );

            log_print( "[gps] sample %d/%d  lat=%.7f lon=%.7f alt=%.1fm"
                       "  hAcc=%.1fm  σ=%.2fm  fix=%s  sats=%d\n",
                       lat_w.n, MAX_SAMPLES,
                       lat_w.mean, lon_w.mean, alt_w.mean,
                       sigma_m, lat_w.stddev(),
                       s_driver->fix_label().data(), c.satellites );

            if ( lat_w.n >= MIN_SAMPLES &&
                 ( lat_w.stddev() < CONVERGE_SIGMA_M || lat_w.n >= MAX_SAMPLES ) ) {

                converged = true;
                log_print( "[gps] converged after %d samples:"
                           " lat=%.7f lon=%.7f alt=%.2fm  σ=%.2fm\n",
                           lat_w.n, lat_w.mean, lon_w.mean, alt_w.mean,
                           lat_w.stddev() );
            }

            LocationMsg msg = { lat_w.mean, lon_w.mean, alt_w.mean, time_us_64() };
            xQueueOverwrite( g_gs_location_q, &msg );

        } else {
            // Keep draining UART but only check for large jumps periodically.
            uint64_t now = time_us_64();
            if ( now - last_check_us < (uint64_t)POLL_INTERVAL_CONVERGED_MS * 1000 )
                continue;
            last_check_us = now;

            static constexpr double RESET_JUMP_M = 50.0;
            double dlat = (c.latitude  - lat_w.mean) * 111320.0;
            double dlon = (c.longitude - lon_w.mean) * 111320.0
                          * cos( lat_w.mean * M_PI / 180.0 );
            double jump = sqrt( dlat*dlat + dlon*dlon );

            if ( jump > RESET_JUMP_M ) {
                log_print( "[gps] position jump %.1fm — resetting average\n", jump );
                lat_w = {}; lon_w = {}; alt_w = {};
                converged = false;
            } else {
                log_print( "[gps] drift check ok (jump=%.2fm)\n", jump );
            }
        }
    }
}

static StaticTask_t s_gps_tcb;
static StackType_t  s_gps_stack[ 2048 ];

void gps_task_init()
{
    TaskHandle_t h = task_create( gps_task, "gps", 2048, nullptr,
                                  tskIDLE_PRIORITY + 3,
                                  s_gps_stack, &s_gps_tcb );
    vTaskCoreAffinitySet( h, 1u << 0 );
}
