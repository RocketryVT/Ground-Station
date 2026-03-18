#pragma once

// LoRa1 task – SX1276 / 915 MHz on SPI0.
//
// Initialises the SX1276 radio on SPI0 then continuously listens for
// SIGMA-framed telemetry packets from the rocket.  For each valid frame:
//   • Logs a CSV row via log_print().
//   • Publishes JSON to g_mqtt_queue under "rocket/lora".
//   • Updates g_rocket_location_q when the packet carries a valid GPS fix.
void lora1_task_init();
