#include "wifi_link.h"

#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_link";
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_link_status_t status = {.state = WIFI_LINK_NEEDS_CONFIG};
static esp_timer_handle_t reconnect_timer;
static atomic_bool stopping;

wifi_link_status_t wifi_link_status(void)
{
    portENTER_CRITICAL(&status_lock);
    wifi_link_status_t copy = status;
    portEXIT_CRITICAL(&status_lock);
    return copy;
}

static void set_disconnected(wifi_link_state_t state)
{
    portENTER_CRITICAL(&status_lock);
    status = (wifi_link_status_t){.state = state};
    portEXIT_CRITICAL(&status_lock);
}

static void schedule_reconnect(void)
{
    if (atomic_load(&stopping)) {
        return;
    }
    // Both the timer callback and the event loop may request the same retry.
    esp_err_t result = esp_timer_start_once(reconnect_timer, 5 * 1000 * 1000);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(result);
    }
}

static void connect_wifi(void *arg)
{
    if (atomic_load(&stopping)) {
        return;
    }
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Connection request failed: %s; retry in 5s", esp_err_to_name(result));
        schedule_reconnect();
    }
}

static void on_network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (atomic_load(&stopping)) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        connect_wifi(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        set_disconnected(WIFI_LINK_DISCONNECTED);
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Disconnected (reason %u); retry in 5s", event->reason);
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        set_disconnected(WIFI_LINK_DISCONNECTED);
        // Restart association if the link stays up but DHCP has lost its address.
        ESP_ERROR_CHECK(esp_wifi_disconnect());
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        wifi_link_status_t next = {
            .state = WIFI_LINK_CONNECTED,
            .connected_us = esp_timer_get_time(),
        };
        snprintf(next.ip, sizeof(next.ip), IPSTR, IP2STR(&event->ip_info.ip));
        portENTER_CRITICAL(&status_lock);
        status = next;
        portEXIT_CRITICAL(&status_lock);
        esp_err_t result = esp_timer_stop(reconnect_timer);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(result);
        }
        ESP_LOGI(TAG, "Device IP: %s", next.ip);
    }
}

bool wifi_link_start(void)
{
    size_t ssid_length = strlen(CONFIG_PC_MONITOR_WIFI_SSID);
    size_t password_length = strlen(CONFIG_PC_MONITOR_WIFI_PASSWORD);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64 ||
        (password_length > 0 && password_length < 8)) {
        ESP_LOGE(TAG, "Set a valid WiFi name and password in menuconfig > CPR Monitor");
        return false;
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *interface = esp_netif_create_default_wifi_sta();
    assert(interface);
    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    const esp_timer_create_args_t timer_config = {
        .callback = connect_wifi,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_config, &reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               on_network_event, NULL));
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, CONFIG_PC_MONITOR_WIFI_SSID, ssid_length);
    memcpy(config.sta.password, CONFIG_PC_MONITOR_WIFI_PASSWORD, password_length);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    set_disconnected(WIFI_LINK_CONNECTING);
    ESP_ERROR_CHECK(esp_wifi_start());
    return true;
}

void wifi_link_stop(void)
{
    atomic_store(&stopping, true);
    esp_err_t result = esp_timer_stop(reconnect_timer);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(result);
    }
    ESP_ERROR_CHECK(esp_wifi_stop());
}
