#pragma once
#include "board_battery.h"
#include "wifi_link.h"
void cpr_ui_init(void);
void cpr_ui_update(const wifi_link_status_t *network, const board_battery_status_t *battery, int shutdown);
void cpr_ui_select_page(unsigned page);
