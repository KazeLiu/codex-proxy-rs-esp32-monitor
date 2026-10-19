#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_LINK_NEEDS_CONFIG,
    WIFI_LINK_CONNECTING,
    WIFI_LINK_DISCONNECTED,
    WIFI_LINK_CONNECTED,
} wifi_link_state_t;

typedef struct {
    wifi_link_state_t state;
    char ip[16];
    int64_t connected_us;
} wifi_link_status_t;

bool wifi_link_start(void);
void wifi_link_stop(void);
wifi_link_status_t wifi_link_status(void);
