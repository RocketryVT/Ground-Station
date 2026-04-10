#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Background IRQ mode — used with pico_cyw43_arch_lwip_threadsafe_background.
// lwIP runs in IRQ context; protect calls from FreeRTOS tasks with
// cyw43_arch_lwip_begin() / cyw43_arch_lwip_end().
//
// We do NOT enable the socket/netconn APIs (NO_SYS=1 doesn't support them),
// and avoiding lwip/sockets.h prevents macro collisions with RadioLib's
// read()/write() method names.

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0

// Memory
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        4000
#define MEMP_NUM_PBUF                   24
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_SEG                16
#define PBUF_POOL_SIZE                  24
#define PBUF_POOL_BUFSIZE               1024
// Default is auto-calculated from enabled features (~6) but does not include
// MQTT client timers.  Set explicitly with headroom for DHCP+ARP+DNS+TCP+MQTT.
#define MEMP_NUM_SYS_TIMEOUT            16

// Protocol support
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1
#define LWIP_IPV4                       1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_DNS                        1

// Checksum offload hooks (CYW43 can offload some checksums)
#define LWIP_CHECKSUM_CTRL_PER_NETIF    1

// Disable stats/debug to save RAM/flash
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0

// -- SNTP ----------------------------------------------------------------------
// Route SNTP time callbacks to our ntp_task handler.
// sntp_set_system_time_us() is defined in ntp_task.cpp (extern "C").
// Forward declaration required so sntp.c can see it when the macro expands.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void sntp_set_system_time_us(uint32_t sec, uint32_t us);
#ifdef __cplusplus
}
#endif
#define SNTP_SET_SYSTEM_TIME_US(sec, us)  sntp_set_system_time_us((sec), (us))
#define SNTP_UPDATE_DELAY                 60000   // re-sync every 60 s
#define SNTP_STARTUP_DELAY                0       // sync immediately on init
#define SNTP_MAX_SERVERS                  1
#define SNTP_SERVER_DNS                   1       // resolve server by hostname

#endif /* _LWIPOPTS_H */
