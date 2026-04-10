#pragma once

// SX1276 / 915 MHz receive task.
// On each received packet, immediately encodes an INTER_PICO SIGMA frame
// and enqueues it to g_udp_queue for forwarding to the primary Pico.
void lora1_task_init();
