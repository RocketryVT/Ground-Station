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

#endif /* _LWIPOPTS_H */
