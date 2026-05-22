#pragma once

// LoRa0 task - RFM9x / SX1276 / 915 MHz on SPI0.
//
// Initialises the RFM9x radio on SPI0 then continuously listens for
// SIGMA-framed telemetry packets from the rocket.  For each valid frame:
//   - Logs a CSV row via log_print().
//   - Publishes JSON to g_mqtt_queue under "rocket/lora0".
//   - Updates g_rocket_location_q when the packet carries a valid GPS fix.
void lora0_task_init();
