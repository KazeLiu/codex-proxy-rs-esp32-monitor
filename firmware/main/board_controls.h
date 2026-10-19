#pragma once

#include <stdbool.h>

void board_controls_init(void);
// Call after display initialization, before showing the screen.
void board_controls_wait_for_start(void);
// Returns true after a long PWR press is released, requesting shutdown.
bool board_controls_poll(void);
// -1 means hidden; otherwise 0-100 until release or cancellation.
int board_controls_shutdown_progress(void);
void board_controls_power_off(void) __attribute__((noreturn));
