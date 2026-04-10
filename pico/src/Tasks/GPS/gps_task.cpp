#include "gps_task.hpp"
#include "shared.hpp"
#include "../Starlink/starlink.hpp"

#include <math.h>

// ── Averaging parameters ──────────────────────────────────────────────────────
// Poll every 5 s while averaging, then back off to 60 s for drift checks.
// Collect at least MIN_SAMPLES before declaring convergence.
// Declare converged when the standard deviation of recent samples drops below
// CONVERGE_SIGMA_M metres — at that point the estimate is stable and we freeze.
// MAX_SAMPLES caps memory and averaging time (~10 min at 5 s/sample).

#define POLL_INTERVAL_AVERAGING_MS  5000
#define POLL_INTERVAL_CONVERGED_MS  60000

#define MIN_SAMPLES      12           // at least 1 minute of data
#define MAX_SAMPLES      120          // cap at 10 minutes
#define CONVERGE_SIGMA_M 1.5          // stop averaging below 1.5 m std-dev

// ── Welford online mean/variance (numerically stable, single pass) ─────────────
// Tracks lat, lon, alt independently.  We weight each sample by 1/sigma²
// (the horizontal accuracy reported by Starlink) so noisier fixes contribute
// less to the estimate.

struct Welford {
    double w_sum  = 0.0;  // sum of weights
    double mean   = 0.0;  // weighted mean
    double M2     = 0.0;  // weighted sum of squared deviations (for variance)
    int    n      = 0;    // sample count

    void update(double x, double w) {
        // West (1979) weighted Welford update
        w_sum     += w;
        double old = mean;
        mean      += (w / w_sum) * (x - old);
        M2        += w * (x - old) * (x - mean);
        ++n;
    }

    // Population std-dev of the weighted mean estimate
    double stddev() const {
        if (n < 2 || w_sum == 0.0) return 1e9;
        return sqrt(M2 / w_sum);
    }
};

static StarlinkClient s_starlink;

static void gps_task( void* )
{
    log_print( "[gps] started — averaging ground station position\n" );

    xEventGroupWaitBits( g_net_events, EVT_WIFI_CONNECTED,
                         pdFALSE, pdTRUE, portMAX_DELAY );

    Welford lat_w, lon_w, alt_w;
    bool converged = false;

    for ( ;; ) {
        StarlinkLocation loc;
        if ( !s_starlink.get_location( &loc ) ) {
            log_print( "[gps] get_location failed\n" );
            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_AVERAGING_MS ) );
            continue;
        }

        // Skip fixes with poor accuracy or no GPS lock
        if ( loc.source == PositionSource::NONE || loc.sigma_m <= 0.0 ) {
            log_print( "[gps] fix not ready (source=%d sigma=%.1f)\n",
                       (int)loc.source, loc.sigma_m );
            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_AVERAGING_MS ) );
            continue;
        }

        if ( !converged ) {
            // Weight = 1 / sigma²  (lower accuracy → less influence)
            double w = 1.0 / (loc.sigma_m * loc.sigma_m);
            lat_w.update( loc.lat, w );
            lon_w.update( loc.lon, w );
            alt_w.update( loc.alt, w );

            int    n      = lat_w.n;
            double stddev = lat_w.stddev();   // lat and lon track similarly

            log_print( "[gps] sample %d/%d  lat=%.7f lon=%.7f alt=%.1f m"
                       "  σ_fix=%.1f m  σ_est=%.2f m\n",
                       n, MAX_SAMPLES,
                       lat_w.mean, lon_w.mean, alt_w.mean,
                       loc.sigma_m, stddev );

            if ( n >= MIN_SAMPLES &&
                 ( stddev < CONVERGE_SIGMA_M || n >= MAX_SAMPLES ) ) {

                converged = true;
                log_print( "[gps] converged after %d samples: "
                           "lat=%.7f lon=%.7f alt=%.2f m  σ=%.2f m\n",
                           n, lat_w.mean, lon_w.mean, alt_w.mean, stddev );
            }

            // Publish running estimate even before convergence so the
            // antenna tracker has something to work with from the start.
            LocationMsg msg = { lat_w.mean, lon_w.mean, alt_w.mean };
            xQueueOverwrite( g_gs_location_q, &msg );

            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_AVERAGING_MS ) );

        } else {
            // Converged — just sanity-check periodically for large jumps
            // (e.g. if someone moves the ground station between sessions).
            // If the new fix deviates by more than RESET_JUMP_M, restart.
            static constexpr double RESET_JUMP_M = 50.0;
            double dlat = (loc.lat - lat_w.mean) * 111320.0;
            double dlon = (loc.lon - lon_w.mean) * 111320.0 * cos(lat_w.mean * M_PI / 180.0);
            double jump = sqrt(dlat*dlat + dlon*dlon);

            if ( jump > RESET_JUMP_M ) {
                log_print( "[gps] position jump %.1f m — resetting average\n", jump );
                lat_w = {}; lon_w = {}; alt_w = {};
                converged = false;
            } else {
                log_print( "[gps] drift check ok (jump=%.2f m)\n", jump );
            }

            vTaskDelay( pdMS_TO_TICKS( POLL_INTERVAL_CONVERGED_MS ) );
        }
    }
}

static StaticTask_t s_gps_tcb;
static StackType_t  s_gps_stack[ 1536 ];

void gps_task_init()
{
    task_create( gps_task, "gps", 1536, nullptr, tskIDLE_PRIORITY + 2,
                  s_gps_stack, &s_gps_tcb );
}
