// udp_send_task.cpp — drain g_udp_queue, send each frame to primary Pico via UDP.
//
// Uses lwIP raw UDP in NO_SYS=1 / threadsafe-background mode.
// All lwIP calls are wrapped in cyw43_arch_lwip_begin/end.
//
// The primary Pico's IP must be known in advance (set via DHCP static lease or
// hard-coded in shared.hpp as PRIMARY_PICO_IP).  No DNS lookup required.
//
// Thread safety: udp_sendto() is called from within lwip_begin/end, which
// protects against concurrent IRQ-context lwIP operations.

#include "udp_send_task.hpp"
#include "shared.hpp"

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

static void udp_send_task( void* )
{
    log_print( "[udp] task started — waiting for WiFi\n" );

    // Block until WiFi is up
    xEventGroupWaitBits( g_net_events, EVT_WIFI_CONNECTED,
                         pdFALSE, pdTRUE, portMAX_DELAY );

    // Resolve destination address once
    ip_addr_t dest_ip;
    if ( !ip4addr_aton( PRIMARY_PICO_IP, &dest_ip ) ) {
        log_print( "[udp] invalid PRIMARY_PICO_IP '%s' — task halting\n",
                   PRIMARY_PICO_IP );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }

    // Allocate UDP PCB
    cyw43_arch_lwip_begin();
    struct udp_pcb* pcb = udp_new();
    cyw43_arch_lwip_end();

    if ( !pcb ) {
        log_print( "[udp] udp_new() failed — task halting\n" );
        for ( ;; ) vTaskDelay( portMAX_DELAY );
    }

    log_print( "[udp] ready — sending to %s:%u\n",
               PRIMARY_PICO_IP, (unsigned)INTER_PICO_PORT );

    for ( ;; ) {
        UdpFrame frame;

        // Block until a frame is available (up to 100 ms, then re-check link)
        if ( xQueueReceive( g_udp_queue, &frame, pdMS_TO_TICKS( 100 ) ) != pdTRUE ) {
            // Optionally check if WiFi dropped here
            continue;
        }

        if ( frame.len == 0 ) continue;

        cyw43_arch_lwip_begin();

        struct pbuf* p = pbuf_alloc( PBUF_TRANSPORT, frame.len, PBUF_RAM );
        if ( p ) {
            memcpy( p->payload, frame.data, frame.len );
            err_t err = udp_sendto( pcb, p, &dest_ip, INTER_PICO_PORT );
            pbuf_free( p );
            if ( err != ERR_OK ) {
                log_print( "[udp] sendto error %d\n", (int)err );
            }
        } else {
            log_print( "[udp] pbuf_alloc failed\n" );
        }

        cyw43_arch_lwip_end();
    }
}

static StaticTask_t s_udp_tcb;
static StackType_t  s_udp_stack[ 1024 ];

void udp_send_task_init()
{
    task_create( udp_send_task, "udp_send", 1024, nullptr, tskIDLE_PRIORITY + 2,
                  s_udp_stack, &s_udp_tcb );
}
