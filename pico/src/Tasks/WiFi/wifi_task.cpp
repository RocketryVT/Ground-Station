#include "wifi_task.hpp"
#include "shared.hpp"

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"

#define WIFI_RECONNECT_DELAY_MS  5000

static void wifi_task( void* param )
{
    ( void ) param;

    if ( cyw43_arch_init() ) {
        log_print( "[wifi] cyw43 init failed — task exiting\n" );
        vTaskDelete( NULL );
        return;
    }

    cyw43_arch_enable_sta_mode();

    for ( ;; ) {
        log_print( "[wifi] connecting to %s...\n", WIFI_SSID );

        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000 );

        if ( err != 0 ) {
            log_print( "[wifi] connect failed (%d), retrying in %d s\n",
                       err, WIFI_RECONNECT_DELAY_MS / 1000 );
            vTaskDelay( pdMS_TO_TICKS( WIFI_RECONNECT_DELAY_MS ) );
            continue;
        }

        cyw43_arch_lwip_begin();
        const char* ip = ip4addr_ntoa( netif_ip4_addr( netif_default ) );
        cyw43_arch_lwip_end();
        log_print( "[wifi] connected — IP: %s\n", ip );

        xEventGroupSetBits( g_net_events, EVT_WIFI_CONNECTED );

        // Monitor link; clear bit and reconnect if it drops
        for ( ;; ) {
            vTaskDelay( pdMS_TO_TICKS( 3000 ) );

            cyw43_arch_lwip_begin();
            bool up = netif_is_link_up( netif_default );
            cyw43_arch_lwip_end();

            if ( !up ) {
                log_print( "[wifi] link lost\n" );
                xEventGroupClearBits( g_net_events, EVT_WIFI_CONNECTED );
                break;
            }
        }

        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

static StaticTask_t s_wifi_tcb;
static StackType_t  s_wifi_stack[ 2048 ];

void wifi_task_init()
{
    configASSERT( xTaskCreateStatic( wifi_task, "wifi", 2048,
                                      NULL, tskIDLE_PRIORITY + 3,
                                      s_wifi_stack, &s_wifi_tcb ) );
}
