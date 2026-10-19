#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

typedef enum {
    CPR_SUMMARY, CPR_MODEL, CPR_ACCOUNT, CPR_KEY, CPR_ERROR, CPR_REQUESTS, CPR_ALERTS,
    CPR_RESOURCE_COUNT
} cpr_resource_t;

typedef struct {
    cJSON *data;
    unsigned version;
    int status;
    bool loading;
    uint32_t updated_ms;
    char error[96];
} cpr_snapshot_t;

typedef struct {
    unsigned version;
    unsigned count;
    char code[96];
    char message[384];
} cpr_notice_t;

typedef struct {
    bool refreshing;
    bool per_round;
    unsigned interval_seconds;
    unsigned completed;
    unsigned version;
    uint32_t next_ms;
} cpr_refresh_state_t;

void cpr_api_start(void);
void cpr_api_stop(void);
void cpr_api_wait_stopped(void);
void cpr_api_refresh_all(void);
void cpr_api_set_refresh_seconds(unsigned seconds);
void cpr_api_set_auto_pages(bool enabled);
void cpr_api_set_display_on(bool on);
void cpr_api_check_day(void);
const cpr_refresh_state_t *cpr_api_refresh_state(void);
const cpr_notice_t *cpr_api_notice(void);
bool cpr_api_auth_denied(void);
void cpr_api_lock(void);
void cpr_api_unlock(void);
const cpr_snapshot_t *cpr_api_snapshot(cpr_resource_t resource);
