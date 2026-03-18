#pragma once

// GPS task – ground-station position from Starlink over WiFi.
//
// Ground-station GPS no longer comes from a dedicated UART module.
// The Pico 2W connects to the Starlink router and receives position via
// an MQTT subscription on "gs/gps".
//
// TODO: extend the MQTT task to call xQueueOverwrite(g_gs_location_q, &loc)
//       when a "gs/gps" JSON message is received.  Until that is wired up,
//       g_gs_location_q will remain empty and the servo controller will hold
//       its last known position.
void gps_task_init();
