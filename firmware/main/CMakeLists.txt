idf_component_register(
    SRCS "main.c" "board_display.c" "board_controls.c" "board_battery.c"
         "wifi_link.c" "cpr_api.c" "cpr_ui.c"
         "cpr_misans_12.c" "cpr_misans_14.c" "cpr_misans_32.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../.private/api-key.txt"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_usb_serial_jtag
             esp_lcd esp_psram esp_hw_support esp_timer esp_wifi esp_event
             esp_netif nvs_flash esp_http_client esp-tls mbedtls esp_adc lwip
             lvgl esp_lcd_axs15231b esp_io_expander_tca9554 cjson
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-finput-charset=UTF-8")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
