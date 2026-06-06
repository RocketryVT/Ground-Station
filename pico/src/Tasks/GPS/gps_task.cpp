#include "gps_task.hpp"
#include "shared.hpp"

// Pico SDK headers before gps_driver.hpp (see gps_driver.hpp transport section).
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#include "gps/gps_driver.hpp"

#include <math.h>

// ── Averaging parameters ──────────────────────────────────────────────────────
// Collect a running Welford average until the estimate stabilises, then freeze
// and do periodic drift checks.  Same tuning as the old Starlink path.

#define POLL_INTERVAL_MS         10
#define POLL_INTERVAL_CONVERGED_MS 60000

#define MIN_SAMPLES      12
#define MAX_SAMPLES      120
#define CONVERGE_SIGMA_M 1.5

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
    log_print( "[gps] u-blox M10 — uart0 GPIO%u/%u  38400 baud\n",
               Pins::GPS_TX, Pins::GPS_RX );

    gps::UartTransport uart( uart0, Pins::GPS_TX, Pins::GPS_RX, 38400 );
    gps::GpsDriver     driver( uart );

    // Configure M10 via CFG-VALSET (Gen 9/10 interface).
    // Send at boot baud (38400 default); module applies immediately.
    driver.send_ubx( gps::Ubx::valset_uart1_inprot_ubx( true ) );
    driver.send_ubx( gps::Ubx::valset_uart1_outprot_ubx( true ) );
    driver.send_ubx( gps::Ubx::valset_uart1_outprot_nmea( false ) );
    driver.send_ubx( gps::Ubx::valset_nav_pvt_uart1( 1 ) );
    driver.send_ubx( gps::Ubx::valset_rate_meas( 1000 ) );   // 1 Hz
    driver.send_ubx( gps::Ubx::valset_dyn_model( 2 ) );      // stationary
    vTaskDelay( pdMS_TO_TICKS( 200 ) );

    Welford lat_w, lon_w, alt_w;
    bool     converged     = false;
    uint32_t last_pvt      = 0;
    uint64_t last_check_us = 0;

    for ( ;; ) {
        driver.poll_ubx_only();

        const uint32_t pvt_count = driver.diagnostics().ubx_pvt;
        if ( pvt_count == last_pvt ) {
            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_MS ) );
            continue;
        }
        last_pvt = pvt_count;

        if ( !driver.has_fix() ) {
            log_print( "[gps] waiting for fix (%s)\n",
                       driver.fix_label().data() );
            continue;
        }

        const auto&  c       = driver.coordinate();
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
                       driver.fix_label().data(), c.satellites );

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
    task_create( gps_task, "gps", 2048, nullptr, tskIDLE_PRIORITY + 2,
                 s_gps_stack, &s_gps_tcb );
}
