#pragma once

#include <stdbool.h>

typedef struct {
    bool available;
    int millivolts;
    int percent;
} board_battery_status_t;

void board_battery_init(void);
// Samples every five seconds; safe to call from each main-loop iteration.
board_battery_status_t board_battery_poll(void);
