#pragma once

// Spawns the LoRa receive task.
// Initialises the SX1276 on SPI0, then continuously listens for SIGMA-framed
// LoRa packets from the rocket.
// Each decoded packet is:
//   1. Logged as a CSV row via log_print() for serial monitoring.
//   2. Published as a JSON payload on g_mqtt_queue under "rocket/lora".
void lora_task_init();
void lora_task( void* param );
