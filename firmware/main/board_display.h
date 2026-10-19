#pragma once

#include <stdbool.h>
#include <stdint.h>

// Waveshare ESP32-S3-Touch-LCD-3.49B V2, software-rotated landscape 640 x 172.
void board_display_init(void);
void board_display_backlight_on(void);
void board_display_backlight_off(void);
bool board_display_is_on(void);
void board_display_toggle_backlight(void);
void board_display_cycle_brightness(void);
void board_display_release_power_hold(void);
bool board_display_is_touched(void);
uint32_t board_display_last_touch(void);
void board_display_dump_frame(void);
