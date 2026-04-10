#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Background IRQ mode — used with pico_cyw43_arch_lwip_threadsafe_background.
// lwIP runs in IRQ context; protect calls from FreeRTOS tasks with
// cyw43_arch_lwip_begin() / cyw43_arch_lwip_end().
//
// Secondary Pico only needs UDP (no TCP, no MQTT, no SNTP).

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0

// Memory — smaller than primary; only UDP forwarding needed
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        3000
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_RAW_PCB                2
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                0
#define MEMP_NUM_TCP_SEG                0
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               256
#define MEMP_NUM_SYS_TIMEOUT            8

// Protocol support
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_DHCP                       1
#define LWIP_IPV4                       1
#define LWIP_TCP                        0
#define LWIP_UDP                        1
#define LWIP_DNS                        0

// Checksum offload hooks (CYW43 can offload some checksums)
#define LWIP_CHECKSUM_CTRL_PER_NETIF    1

// Disable stats/debug to save RAM/flash
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0

#endif /* _LWIPOPTS_H */
