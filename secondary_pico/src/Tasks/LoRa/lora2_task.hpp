#pragma once

// RFM69HCW / 433 MHz receive task.
// On each received packet, immediately encodes an INTER_PICO SIGMA frame
// and enqueues it to g_udp_queue for forwarding to the primary Pico.
void lora2_task_init();
