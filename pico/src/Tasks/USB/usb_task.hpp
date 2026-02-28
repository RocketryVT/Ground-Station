#pragma once

// Spawns the USB console task.
// Prints periodic status (heap, WiFi, MQTT) over USB CDC stdio.
// stdio_init_all() must be called before vTaskStartScheduler().
void usb_task_init();
