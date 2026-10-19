#include "cpr_api.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_link.h"

extern const char api_key_start[] asm("_binary_api_key_txt_start");
static SemaphoreHandle_t lock, exited;
static cpr_snapshot_t snapshots[CPR_RESOURCE_COUNT];
static bool requested = true;
static bool display_on = true;
static cpr_refresh_state_t refresh = {.refreshing = true, .interval_seconds = 30};
static atomic_bool stopping;
static bool denied;
static bool notice_baseline, clock_known;
static int server_offset;
static int64_t current_day;
static char seen_errors[10][96];
static unsigned seen_error_count;
static cpr_notice_t notice;

typedef struct { char *bytes; size_t length, capacity; } response_t;
static uint32_t now_ms(void) { return esp_timer_get_time() / 1000; }
void cpr_api_lock(void) { xSemaphoreTake(lock, portMAX_DELAY); }
void cpr_api_unlock(void) { xSemaphoreGive(lock); }
const cpr_snapshot_t *cpr_api_snapshot(cpr_resource_t resource) { return &snapshots[resource]; }
const cpr_notice_t *cpr_api_notice(void) { return &notice; }
const cpr_refresh_state_t *cpr_api_refresh_state(void) { return &refresh; }
bool cpr_api_auth_denied(void) { return denied; }

static void copy_utf8(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    if (length >= capacity) length = capacity - 1;
    while (length && ((unsigned char)source[length] & 0xc0) == 0x80) --length;
    memcpy(destination, source, length);
    destination[length] = 0;
}

static void *json_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t receive_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    response_t *body = event->user_data;
    size_t needed = body->length + event->data_len + 1;
    if (needed > 2 * 1024 * 1024) return ESP_ERR_INVALID_SIZE;
    if (needed > body->capacity) {
        size_t capacity = needed * 2;
        char *bytes = heap_caps_realloc(body->bytes, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bytes) return ESP_ERR_NO_MEM;
        body->bytes = bytes;
        body->capacity = capacity;
    }
    memcpy(body->bytes + body->length, event->data, event->data_len);
    body->length += event->data_len;
    body->bytes[body->length] = 0;
    return ESP_OK;
}

static cJSON *read_api(esp_http_client_handle_t client, const char *path, int *status)
{
    if (!client) { *status = -ESP_ERR_NO_MEM; return NULL; }
    char url[512];
    snprintf(url, sizeof(url), "%s%s", CONFIG_CPR_BASE_URL, path);
    response_t body = {0};
    esp_http_client_set_user_data(client, &body);
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_perform(client);
    *status = err == ESP_OK ? esp_http_client_get_status_code(client) : -(int)err;
    if (err != ESP_OK) esp_http_client_close(client);
    esp_http_client_set_user_data(client, NULL);
    cJSON *envelope = body.bytes ? cJSON_ParseWithLength(body.bytes, body.length) : NULL;
    free(body.bytes);
    cJSON *data = NULL;
    if (*status == 200 && cJSON_IsObject(envelope)) {
        data = cJSON_DetachItemFromObjectCaseSensitive(envelope, "data");
        if (!cJSON_IsObject(data)) { cJSON_Delete(data); data = NULL; *status = -ESP_ERR_INVALID_RESPONSE; }
    }
    cJSON_Delete(envelope);
    return data;
}

static const char *string_field(cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static bool add_account_notes(esp_http_client_handle_t client, cJSON *diagnostics, int *status)
{
    int page = 1, total_pages;
    do {
        if (atomic_load(&stopping)) return false;
        char path[96];
        snprintf(path, sizeof(path), "/api/admin/accounts?page=%d&pageSize=20", page);
        cJSON *accounts = read_api(client, path, status);
        if (!accounts) return false;
        cJSON *account;
        cJSON_ArrayForEach(account, cJSON_GetObjectItemCaseSensitive(accounts, "items")) {
            cJSON *row;
            cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(diagnostics, "items")) {
                if (strcmp(string_field(row, "key"), string_field(account, "id")) == 0) {
                    cJSON_AddStringToObject(row, "accountNotes", string_field(account, "notes"));
                }
            }
        }
        cJSON *pagination = cJSON_GetObjectItemCaseSensitive(accounts, "page");
        cJSON *total = cJSON_GetObjectItemCaseSensitive(pagination, "totalPages");
        total_pages = cJSON_IsNumber(total) ? total->valueint : 0;
        cJSON_Delete(accounts);
    } while (++page <= total_pages);
    return true;
}

static void collect_new_errors(cJSON *data)
{
    if (refresh.per_round) {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
        unsigned count = cJSON_GetArraySize(items);
        if (count > 10) count = 10;
        unsigned fresh = 0;
        cJSON *latest = NULL;
        for (unsigned i = 0; i < count; ++i) {
            cJSON *row = cJSON_GetArrayItem(items, i);
            const char *id = string_field(row, "id");
            bool known = false;
            for (unsigned j = 0; j < seen_error_count; ++j) {
                if (strcmp(id, seen_errors[j]) == 0) { known = true; break; }
            }
            if (notice_baseline && id[0] && !known) {
                ++fresh;
                if (!latest) latest = row;
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            copy_utf8(seen_errors[i], sizeof(seen_errors[i]), string_field(cJSON_GetArrayItem(items, i), "id"));
        }
        seen_error_count = count;
        notice_baseline = true;
        if (latest) {
            const char *code = string_field(latest, "kind");
            const char *failure_class = string_field(latest, "failureClass");
            if (failure_class[0]) code = failure_class;
            const char *provider_code = string_field(latest, "providerErrorCode");
            if (provider_code[0]) code = provider_code;
            const char *message = string_field(latest, "message");
            if (!message[0]) message = string_field(latest, "errorDetails");
            notice.count = fresh;
            copy_utf8(notice.code, sizeof(notice.code), code);
            copy_utf8(notice.message, sizeof(notice.message), message);
            ++notice.version;
        }
    }
}

static bool parse_datetime(const char *value, time_t *result)
{
    int year, month, day, hour, minute, second;
    if (sscanf(value, "%d-%d-%d%*c%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) return false;
    struct tm date = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                      .tm_hour = hour, .tm_min = minute, .tm_sec = second};
    *result = mktime(&date);
    return *result != (time_t)-1;
}

static int64_t server_day(time_t timestamp) { return ((int64_t)timestamp + server_offset) / 86400; }

void cpr_api_check_day(void)
{
    cpr_api_lock();
    if (!clock_known || server_day(time(NULL)) == current_day) {
        cpr_api_unlock();
        return;
    }
    current_day = server_day(time(NULL));
    for (unsigned i = 0; i < CPR_RESOURCE_COUNT; ++i) {
        cJSON_Delete(snapshots[i].data);
        unsigned version = snapshots[i].version + 1;
        snapshots[i] = (cpr_snapshot_t){.version = version};
    }
    notice_baseline = false;
    seen_error_count = 0;
    requested = refresh.refreshing = true;
    cpr_api_unlock();
    ESP_LOGI("cpr", "CPR date changed; yesterday's snapshots cleared");
}

static bool update_server_clock(cJSON *summary)
{
    time_t utc, display;
    if (!parse_datetime(string_field(summary, "asOf"), &utc) ||
        !parse_datetime(string_field(summary, "asOfDisplay"), &display)) return false;
    cpr_api_lock();
    server_offset = (int)(display - utc);
    if (!clock_known) {
        current_day = server_day(time(NULL));
        clock_known = true;
    }
    cpr_api_unlock();
    return true;
}

static bool update(esp_http_client_handle_t client, cpr_resource_t resource, time_t as_of)
{
    cpr_api_lock();
    snapshots[resource].loading = true;
    cpr_api_unlock();
    char path[256];
    long long anchor = (long long)as_of * 1000;
    if (resource == CPR_SUMMARY) {
        snprintf(path, sizeof(path), "/api/admin/dashboard/summary?period=today&kind=usage&asOf=%lld", anchor);
    } else if (resource <= CPR_ERROR) {
        const char *dimensions[] = {"", "model", "account", "apiKey", "failureClass"};
        snprintf(path, sizeof(path), "/api/admin/usage/insights/diagnostics?period=today&asOf=%lld&dimension=%s",
                 anchor, dimensions[resource]);
    } else if (resource == CPR_REQUESTS) {
        snprintf(path, sizeof(path), "/api/admin/usage/records?period=today&asOf=%lld&currentPage=1&pageSize=10&status=all", anchor);
    } else {
        snprintf(path, sizeof(path), "/api/admin/operations/errors?period=today&asOf=%lld&currentPage=1&pageSize=10", anchor);
    }
    int status;
    cJSON *data = read_api(client, path, &status);
    if (data && resource == CPR_SUMMARY && !update_server_clock(data)) {
        cJSON_Delete(data);
        data = NULL;
        status = -ESP_ERR_INVALID_RESPONSE;
    }
    if (data && resource == CPR_ACCOUNT && !add_account_notes(client, data, &status)) {
        cJSON_Delete(data);
        data = NULL;
    }
    cpr_api_check_day();
    cpr_api_lock();
    if (clock_known && server_day(as_of) != current_day) {
        cpr_api_unlock();
        cJSON_Delete(data);
        return false;
    }
    cpr_snapshot_t *snapshot = &snapshots[resource];
    snapshot->loading = false;
    snapshot->status = status;
    snapshot->version++;
    refresh.completed = resource + 1;
    if (data) {
        cJSON_Delete(snapshot->data);
        snapshot->data = data;
        snapshot->updated_ms = now_ms();
        snapshot->error[0] = 0;
        if (resource == CPR_ALERTS) collect_new_errors(data);
    } else if (status == 401 || status == 403) {
        denied = true;
        snprintf(snapshot->error, sizeof(snapshot->error), "鉴权失败 HTTP %d", status);
    } else {
        snprintf(snapshot->error, sizeof(snapshot->error), status > 0 ? "接口失败 HTTP %d" : "连接失败 (%d)", status);
    }
    cpr_api_unlock();
    ESP_LOGI("cpr", "Resource %d: HTTP/result %d, %s", resource, status, data ? "updated" : "failed");
    return true;
}

static void refresh_all(void)
{
    cpr_api_lock();
    requested = false;
    refresh.completed = 0;
    uint32_t started = now_ms();
    refresh.next_ms = started + refresh.interval_seconds * 1000;
    cpr_api_unlock();
    time_t as_of = time(NULL);
    const esp_http_client_config_t config = {
        .url = CONFIG_CPR_BASE_URL, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = receive_event,
        .buffer_size = 4096, .buffer_size_tx = 2048, .disable_auto_redirect = true,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_header(client, "x-api-key", api_key_start);
        esp_http_client_set_header(client, "Accept", "application/json");
    }
    for (int i = 0; i < CPR_RESOURCE_COUNT && !atomic_load(&stopping); ++i) {
        if (!update(client, i, as_of) || denied || wifi_link_status().state != WIFI_LINK_CONNECTED) break;
    }
    if (client) esp_http_client_cleanup(client);
    cpr_api_lock();
    refresh.refreshing = requested;
    ++refresh.version;
    if ((int32_t)(now_ms() - refresh.next_ms) >= 0) refresh.next_ms = now_ms() + refresh.interval_seconds * 1000;
    cpr_api_unlock();
    ESP_LOGI("cpr", "Full refresh completed in %lu ms", (unsigned long)(now_ms() - started));
}

static void worker(void *argument)
{
    bool clock_started = false;
    while (!atomic_load(&stopping)) {
        cpr_api_check_day();
        if (!clock_started && wifi_link_status().state == WIFI_LINK_CONNECTED) {
            esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            ESP_ERROR_CHECK(esp_netif_sntp_init(&clock));
            clock_started = true;
        }
        if (wifi_link_status().state == WIFI_LINK_CONNECTED && time(NULL) > 1735689600 && !denied) {
            cpr_api_lock();
            uint32_t now = now_ms();
            bool timed = !refresh.per_round || !display_on;
            if (!refresh.refreshing && timed && (int32_t)(now - refresh.next_ms) >= 0) requested = refresh.refreshing = true;
            bool pending = requested;
            cpr_api_unlock();
            if (pending) refresh_all();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (clock_started) esp_netif_sntp_deinit();
    xSemaphoreGive(exited);
    vTaskDelete(NULL);
}

void cpr_api_start(void)
{
    lock = xSemaphoreCreateMutex();
    exited = xSemaphoreCreateBinary();
    configASSERT(lock && exited);
    setenv("TZ", "UTC0", 1);
    tzset();
    const cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks((cJSON_Hooks *)&hooks);
    configASSERT(xTaskCreate(worker, "cpr_network", 12288, NULL, 4, NULL) == pdPASS);
}
void cpr_api_refresh_all(void)
{
    cpr_api_lock();
    if (!refresh.refreshing && !denied) requested = refresh.refreshing = true;
    cpr_api_unlock();
}
void cpr_api_set_refresh_seconds(unsigned seconds)
{
    cpr_api_lock();
    refresh.interval_seconds = seconds;
    refresh.next_ms = now_ms() + seconds * 1000;
    cpr_api_unlock();
}
void cpr_api_set_auto_pages(bool enabled)
{
    cpr_api_lock();
    if (refresh.per_round != enabled) {
        refresh.per_round = enabled;
        notice_baseline = false;
        seen_error_count = 0;
        refresh.next_ms = now_ms() + refresh.interval_seconds * 1000;
    }
    cpr_api_unlock();
}
void cpr_api_set_display_on(bool on)
{
    cpr_api_lock();
    display_on = on;
    refresh.next_ms = now_ms() + refresh.interval_seconds * 1000;
    cpr_api_unlock();
}
void cpr_api_stop(void) { atomic_store(&stopping, true); }
void cpr_api_wait_stopped(void) { xSemaphoreTake(exited, portMAX_DELAY); }
