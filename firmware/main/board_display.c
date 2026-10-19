/*
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from Waveshare ESP32-S3-Touch-LCD-3.49 V2,
 * 10_LVGL_V9_Test and the SYS_EN setup in 07_BATT_PWR_Test.
 * Modified for this project: minimal board initialization and one LVGL owner.
 * Attribution and license: ../../THIRD_PARTY.md, ../../licenses/Waveshare-Apache-2.0.txt
 */
#include "board_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

#define LCD_WIDTH 172
#define LCD_HEIGHT 640
#define DMA_ROWS 64
#define FRAME_BYTES (LCD_WIDTH * LCD_HEIGHT * 2)
#define DMA_BYTES (LCD_WIDTH * DMA_ROWS * 2)
// V2 backlight PWM is active-low: a constant low level gives full brightness.
#define LCD_BACKLIGHT_GPIO GPIO_NUM_42
#define EXIO_BACKLIGHT (1ULL << 1)
#define EXIO_RESET (1ULL << 5)
#define EXIO_SYS_EN (1ULL << 6)

static esp_io_expander_handle_t expander;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t transfer_done;
static uint8_t *dma_buffer;
static bool backlight_on;
static unsigned brightness_index;
static const unsigned brightness_steps[] = {100, 75, 50, 25};
static uint16_t *logical_frame;
static i2c_master_dev_handle_t touch_device;
static uint32_t last_touch_ms;
static bool touching;

static const axs15231b_lcd_init_cmd_t init_commands[] = {
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 100},
};

static bool on_transfer_done(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_io_event_data_t *event, void *context)
{
    BaseType_t awoken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &awoken);
    return awoken == pdTRUE;
}

static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    // AXS15231B QSPI streams rows using RAMWR/RAMWRC, so always send a full frame.
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = 0; y < LCD_HEIGHT; y += DMA_ROWS) {
        uint16_t *destination = (uint16_t *)dma_buffer;
        for (int row = 0; row < DMA_ROWS; ++row) {
            for (int x = 0; x < LCD_WIDTH; ++x) {
                uint16_t color = source[x * LCD_HEIGHT + LCD_HEIGHT - 1 - y - row];
                destination[row * LCD_WIDTH + x] = (color >> 8) | (color << 8);
            }
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH,
                                                  y + DMA_ROWS, dma_buffer));
        xSemaphoreTake(transfer_done, portMAX_DELAY);
    }
    lv_display_flush_ready(display);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    const uint8_t command[11] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x0e, 0, 0, 0};
    uint8_t reply[32];
    esp_err_t result = i2c_master_transmit_receive(touch_device, command, sizeof(command),
                                                 reply, sizeof(reply), 30);
    touching = result == ESP_OK && reply[1] > 0 && reply[1] < 5;
    data->state = touching ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (touching) {
        unsigned x = ((reply[2] & 15) << 8) | reply[3];
        unsigned y = ((reply[4] & 15) << 8) | reply[5];
        data->point.x = x < 640 ? x : 639;
        data->point.y = y < 172 ? y : 171;
        last_touch_ms = tick_ms();
    }
}

bool board_display_is_touched(void) { return touching; }
uint32_t board_display_last_touch(void) { return last_touch_ms; }

void board_display_dump_frame(void)
{
    flockfile(stdout);
    fflush(stdout);
    const char *header = "CPR_FRAME_BEGIN 640 172\n";
    usb_serial_jtag_write_bytes(header, strlen(header), pdMS_TO_TICKS(1000));
    for (int offset = 0; offset < FRAME_BYTES; offset += 480) {
        unsigned char encoded[648];
        size_t length;
        size_t count = FRAME_BYTES - offset < 480 ? FRAME_BYTES - offset : 480;
        mbedtls_base64_encode(encoded, sizeof(encoded), &length,
                              (uint8_t *)logical_frame + offset, count);
        encoded[length++] = '\n';
        if (usb_serial_jtag_write_bytes(encoded, length, pdMS_TO_TICKS(1000)) != length) {
            funlockfile(stdout);
            return;
        }
    }
    const char *footer = "CPR_FRAME_END\n";
    usb_serial_jtag_write_bytes(footer, strlen(footer), pdMS_TO_TICKS(1000));
    funlockfile(stdout);
}

void board_display_init(void)
{
    ESP_LOGI("display", "3.49B V2, 172x640, PSRAM: %u bytes",
             (unsigned)esp_psram_get_size());
    const ledc_timer_config_t pwm_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 50000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&pwm_timer));
    const ledc_channel_config_t pwm_channel = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_3,
        .duty = 255,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&pwm_channel));

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_47,
        .scl_io_num = GPIO_NUM_48,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, &i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(
        i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &expander));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, EXIO_BACKLIGHT | EXIO_RESET | EXIO_SYS_EN,
                                           IO_EXPANDER_OUTPUT));
    // Latch battery power before LCD setup so releasing PWR does not cut power.
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_SYS_EN, 1));
    ESP_LOGI("power", "Battery power hold enabled (SYS_EN high)");
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BACKLIGHT, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_RESET, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_RESET, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_RESET, 1));
    vTaskDelay(pdMS_TO_TICKS(30));

    transfer_done = xSemaphoreCreateBinary();
    assert(transfer_done);
    const spi_bus_config_t spi_config = {
        .sclk_io_num = GPIO_NUM_10,
        .data0_io_num = GPIO_NUM_11,
        .data1_io_num = GPIO_NUM_12,
        .data2_io_num = GPIO_NUM_13,
        .data3_io_num = GPIO_NUM_14,
        .max_transfer_sz = DMA_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &spi_config, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(GPIO_NUM_9, on_transfer_done, NULL);
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = init_commands,
        .init_cmds_size = sizeof(init_commands) / sizeof(init_commands[0]),
        .flags.use_qspi_interface = true,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(LCD_HEIGHT, LCD_WIDTH);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    uint8_t *frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    logical_frame = (uint16_t *)frame;
    dma_buffer = heap_caps_malloc(DMA_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(frame && dma_buffer);
    lv_display_set_buffers(display, frame, NULL, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_display);

    const i2c_master_bus_config_t touch_bus_config = {
        .i2c_port = I2C_NUM_1, .sda_io_num = GPIO_NUM_17, .scl_io_num = GPIO_NUM_18,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t touch_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&touch_bus_config, &touch_bus));
    const i2c_device_config_t touch_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x3b, .scl_speed_hz = 300000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_bus, &touch_config, &touch_device));
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_touch);
    ESP_LOGI("display", "Landscape 640x172 and V2 touch ready");
}

static void set_backlight(bool enabled)
{
    backlight_on = enabled;
    unsigned percent = brightness_steps[brightness_index];
    unsigned duty = enabled ? 255 - (percent * 255 / 100) : 255;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_BACKLIGHT, enabled ? 1 : 0));
}

void board_display_backlight_on(void)
{
    set_backlight(true);
}

void board_display_backlight_off(void)
{
    set_backlight(false);
}

bool board_display_is_on(void)
{
    return backlight_on;
}

void board_display_toggle_backlight(void)
{
    set_backlight(!backlight_on);
    ESP_LOGI("display", "Screen %s", backlight_on ? "on" : "off");
}

void board_display_cycle_brightness(void)
{
    brightness_index = (brightness_index + 1) % (sizeof(brightness_steps) / sizeof(brightness_steps[0]));
    set_backlight(true);
    ESP_LOGI("display", "Brightness: %u%%", brightness_steps[brightness_index]);
}

void board_display_release_power_hold(void)
{
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, EXIO_SYS_EN, 0));
}
