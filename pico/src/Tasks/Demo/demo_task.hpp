#pragma once

// Spawns a demo task that publishes a heartbeat to "gs/demo" every second.
// Monitor on laptop: mosquitto_sub -h <broker_ip> -t gs/demo
void demo_task_init();
