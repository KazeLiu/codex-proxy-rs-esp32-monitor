#include "board_controls.h"

#include "board_display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define POWER_GPIO GPIO_NUM_16
#define BRIGHTNESS_GPIO GPIO_NUM_0
#define DEBOUNCE_US 30000
#define HOLD_FEEDBACK_US 300000
#define LONG_PRESS_US 2000000
#define SAMPLE_PERIOD_US 10000

typedef enum { BUTTON_NONE, BUTTON_DOWN, BUTTON_UP } button_event_t;

typedef struct {
    gpio_num_t gpio;
    int raw_level;
    int stable_level;
    int64_t changed_us;
    bool armed;
} button_t;

typedef struct {
    gpio_num_t gpio;
    button_event_t edge;
    int64_t occurred_us;
} button_message_t;

static button_t power_button;
static button_t brightness_button;
static QueueHandle_t button_events;
static esp_timer_handle_t button_timer;
static bool power_down;
static int64_t power_pressed_us;
static bool shutdown_pending;
static bool start_requires_hold;
static bool screen_was_on;
static int shutdown_progress = -1;

static button_t init_button(gpio_num_t gpio)
{
    int level = gpio_get_level(gpio);
    return (button_t){
        .gpio = gpio,
        .raw_level = level,
        .stable_level = level,
        .changed_us = esp_timer_get_time(),
        .armed = level != 0,
    };
}

static button_event_t poll_button(button_t *button, int64_t now)
{
    int level = gpio_get_level(button->gpio);
    if (level != button->raw_level) {
        button->raw_level = level;
        button->changed_us = now;
    }
    if (button->raw_level == button->stable_level || now - button->changed_us < DEBOUNCE_US) {
        return BUTTON_NONE;
    }
    button->stable_level = level;
    // The press used to turn the device on must not also turn it off.
    if (!button->armed) {
        button->armed = level != 0;
        return BUTTON_NONE;
    }
    if (level == 0) {
        return BUTTON_DOWN;
    }
    return BUTTON_UP;
}

static void sample_buttons(void *argument)
{
    button_t *buttons[] = {&power_button, &brightness_button};
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < 2; ++i) {
        button_t *button = buttons[i];
        button_event_t edge = poll_button(button, now);
        if (edge == BUTTON_NONE) continue;
        button_message_t event = {.gpio = button->gpio, .edge = edge, .occurred_us = button->changed_us};
        if (xQueueSend(button_events, &event, 0) != pdTRUE) ESP_LOGW("controls", "Button event queue full");
    }
}

void board_controls_init(void)
{
    ESP_ERROR_CHECK(rtc_gpio_deinit(POWER_GPIO));
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << POWER_GPIO) | (1ULL << BRIGHTNESS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));
    bool woke_by_power = (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_EXT0)) != 0;
    start_requires_hold = woke_by_power || gpio_get_level(POWER_GPIO) == 0;
}

void board_controls_wait_for_start(void)
{
    // Capture the initial press before LCD setup so an early release cannot bypass the hold.
    if (start_requires_hold) {
        while (gpio_get_level(POWER_GPIO) == 0 && esp_timer_get_time() < LONG_PRESS_US) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (gpio_get_level(POWER_GPIO) != 0) {
            board_controls_power_off();
        }
    }

    power_button = init_button(POWER_GPIO);
    brightness_button = init_button(BRIGHTNESS_GPIO);
    button_events = xQueueCreate(16, sizeof(button_message_t));
    configASSERT(button_events);
    // Sample outside the UI loop so full-frame drawing cannot swallow a short press.
    const esp_timer_create_args_t timer = {
        .callback = sample_buttons, .name = "buttons", .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &button_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(button_timer, SAMPLE_PERIOD_US));
    ESP_LOGI("controls", "10ms key sampling; PWR: tap screen, hold 2s power off; SUN: brightness");
}

bool board_controls_poll(void)
{
    int64_t now = esp_timer_get_time();
    button_message_t event;
    while (xQueueReceive(button_events, &event, 0) == pdTRUE) {
        if (event.gpio == POWER_GPIO) {
            if (event.edge == BUTTON_DOWN) {
                power_down = true;
                power_pressed_us = event.occurred_us;
            } else {
                power_down = false;
                if (shutdown_pending) return true;
                if (shutdown_progress >= 0) {
                    shutdown_progress = -1;
                    if (!screen_was_on) board_display_backlight_off();
                } else if (event.occurred_us - power_pressed_us < HOLD_FEEDBACK_US) {
                    board_display_toggle_backlight();
                }
            }
        } else if (!shutdown_pending && shutdown_progress < 0 && event.edge == BUTTON_UP) {
            board_display_cycle_brightness();
        }
    }

    if (power_down && gpio_get_level(POWER_GPIO) == 0) {
        int64_t held_us = now - power_pressed_us;
        if (held_us >= HOLD_FEEDBACK_US) {
            if (shutdown_progress < 0) {
                screen_was_on = board_display_is_on();
                board_display_backlight_on();
            }
            shutdown_progress = held_us >= LONG_PRESS_US ? 100 : (int)(held_us * 100 / LONG_PRESS_US);
            if (!shutdown_pending && shutdown_progress == 100) {
                shutdown_pending = true;
                ESP_LOGI("controls", "Power off requested; release PWR");
            }
        }
    }
    return false;
}

int board_controls_shutdown_progress(void)
{
    return shutdown_progress;
}

void board_controls_power_off(void)
{
    if (button_timer) ESP_ERROR_CHECK(esp_timer_stop(button_timer));
    board_display_backlight_off();
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(POWER_GPIO, 0));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(POWER_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(POWER_GPIO));
    ESP_LOGI("controls", "Power off; hold PWR to start");
    board_display_release_power_hold();
    // USB can keep the board powered after SYS_EN is released; sleep until PWR.
    esp_deep_sleep_start();
}
