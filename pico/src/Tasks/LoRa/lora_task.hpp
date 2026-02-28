#pragma once

// Spawns the LoRa receive task.
// Initialises the SX1276 on SPI0, then continuously listens for packets.
// Each received packet is serialised as JSON and pushed onto g_mqtt_queue
// under the topic "rocket/lora".
void lora_task_init();
void lora_task( void* param );
