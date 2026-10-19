#include "board_battery.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#define BATTERY_CHANNEL ADC_CHANNEL_3
#define SAMPLE_COUNT 16
#define SAMPLE_INTERVAL_US 5000000

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static board_battery_status_t status;
static int64_t next_sample_us;

void board_battery_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &adc);
    if (err != ESP_OK) {
        ESP_LOGE("battery", "ADC initialization failed: %s", esp_err_to_name(err));
        return;
    }
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(adc, BATTERY_CHANNEL, &channel);
    if (err == ESP_OK) {
        const adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1,
            .chan = BATTERY_CHANNEL,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        err = adc_cali_create_scheme_curve_fitting(&config, &calibration);
    }
    if (err != ESP_OK) {
        ESP_LOGE("battery", "ADC configuration/calibration failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(adc_oneshot_del_unit(adc));
        return;
    }
    ESP_LOGI("battery", "Calibrated GPIO4 battery monitor ready (3:1 divider)");
}

board_battery_status_t board_battery_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (!calibration || now < next_sample_us) {
        return status;
    }
    next_sample_us = now + SAMPLE_INTERVAL_US;
    int sum_mv = 0;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        int millivolts;
        esp_err_t err = adc_oneshot_get_calibrated_result(adc, calibration,
                                                         BATTERY_CHANNEL, &millivolts);
        if (err != ESP_OK) {
            ESP_LOGE("battery", "Battery reading failed: %s", esp_err_to_name(err));
            status.available = false;
            return status;
        }
        sum_mv += millivolts;
    }
    // V2 schematic: VBAT -> 200 kOhm -> GPIO4 -> 100 kOhm -> GND.
    int battery_mv = (sum_mv * 3 + SAMPLE_COUNT / 2) / SAMPLE_COUNT;
    if (battery_mv < 2500 || battery_mv > 4500) {
        ESP_LOGW("battery", "Battery voltage outside measurement range: %d mV", battery_mv);
        status.available = false;
        return status;
    }
    status.millivolts = status.available ? (status.millivolts * 3 + battery_mv + 2) / 4 : battery_mv;
    // A coarse voltage estimate, not a fuel gauge: 3.30-4.20 V in 5% steps.
    int percent = ((status.millivolts - 3300 + 22) / 45) * 5;
    status.percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    status.available = true;
    return status;
}
