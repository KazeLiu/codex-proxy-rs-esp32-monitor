#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "board_battery.h"
#include "board_controls.h"
#include "board_display.h"
#include "cpr_api.h"
#include "cpr_ui.h"
#include "wifi_link.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

void app_main(void)
{
    board_controls_init();
    board_display_init();
    board_controls_wait_for_start();
    board_battery_init();
    esp_log_level_set("wifi", ESP_LOG_WARN);
    bool wifi_started = wifi_link_start();
    cpr_api_start();
    cpr_ui_init();
    lv_timer_handler();
    board_display_backlight_on();
    usb_serial_jtag_driver_config_t console = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    console.tx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&console));
    usb_serial_jtag_vfs_use_driver();
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI("cpr", "CPR monitor ready: landscape, MiSans Regular, seven pages");
    for (;;) {
        if (board_controls_poll()) {
            cpr_api_stop();
            if (wifi_started) wifi_link_stop();
            cpr_api_wait_stopped();
            board_controls_power_off();
        }
        wifi_link_status_t network = wifi_link_status();
        board_battery_status_t battery = board_battery_poll();
        cpr_api_check_day();
        cpr_ui_update(&network, &battery, board_controls_shutdown_progress());
        lv_timer_handler();
        char command;
        if (read(STDIN_FILENO, &command, 1) == 1) {
            if (command == 'S') board_display_dump_frame();
            if (command >= '1' && command <= '7') cpr_ui_select_page(command - '1');
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
