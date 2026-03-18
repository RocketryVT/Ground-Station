#pragma once

// LoRa2 task – RFM69HCW / 433 MHz on SPI1.
//
// Initialises the RFM69HCW radio on SPI1 and continuously listens for
// packets on the 433 MHz backup / command link.  Received payloads are
// forwarded to g_mqtt_queue under "rocket/rf69".
void lora2_task_init();
