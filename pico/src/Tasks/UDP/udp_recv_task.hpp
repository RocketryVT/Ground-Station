#pragma once

// UDP receive task — primary Pico side.
//
// Listens on port INTER_PICO_PORT for SIGMA INTER_PICO frames forwarded by
// the secondary Pico.  On each valid frame, decodes the InterPicoData and:
//   • Writes the rocket location to g_rocket_location_q (xQueueOverwrite).
//   • Publishes a JSON payload to the MQTT queue (best-effort, non-blocking).
//
// Uses lwIP raw UDP callbacks (NO_SYS=1 / threadsafe-background).
// The callback fires from the lwIP IRQ context and posts frames to an
// internal queue; the FreeRTOS task drains the queue and does all decoding.
void udp_recv_task_init();
