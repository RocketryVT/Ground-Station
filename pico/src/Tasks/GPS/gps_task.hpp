#pragma once

// Spawns the UART GPS polling task.
//
// The GPS device, target baud, and navigation rate come from board_profile.hpp
// through boards/board.hpp. The task writes the averaged ground-station position
// to g_gs_location_q via xQueueOverwrite().
void gps_task_init();
