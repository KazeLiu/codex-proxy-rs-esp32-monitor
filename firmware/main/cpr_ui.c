#include "cpr_ui.h"
#include "cpr_api.h"
#include "board_display.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(cpr_misans_12);
LV_FONT_DECLARE(cpr_misans_14);
LV_FONT_DECLARE(cpr_misans_32);
#define BG 0x0B111C
#define SURFACE 0x0E1726
#define ELEVATED 0x172334
#define SELECTED 0x24385A
#define BORDER 0x2C3C54
#define INK 0xD8E2EE
#define MUTED 0x9AAFC6
#define BLUE 0x5983F4
#define GREEN 0x43C9A2
#define ORANGE 0xE5B56A
#define RED 0xF07B8B
#define ROW_HEIGHT 46
#define PAGE_COUNT 7
#define TABLE_COUNT 5
#define TREND_POINTS 96

enum {
    COUNTER_REQUESTS, COUNTER_TOKENS, COUNTER_HIT_RATE,
    COUNTER_TOTAL_REQUESTS, COUNTER_FIRST_TOKEN,
    COUNTER_TOTAL_TOKENS, COUNTER_COST,
    COUNTER_TOTAL_HIT_RATE, COUNTER_CACHED_TOKENS,
    COUNTER_COUNT
};

static lv_font_t font;
static lv_obj_t *pages[PAGE_COUNT], *connection, *refresh_control, *refresh_text, *battery_text, *page_text;
static lv_obj_t *auto_box, *interval_button, *interval_text, *refresh_interval_button, *refresh_interval_text, *alert;
static lv_obj_t *shutdown_panel, *shutdown_text, *shutdown_bar;
static lv_obj_t *error_toast, *error_title, *error_message;
static unsigned notice_version;
static uint32_t notice_shown_ms;
static lv_obj_t *metric_values[COUNTER_COUNT], *charts[3], *slot_text, *slot_bar;
static int16_t trend_values[3][TREND_POINTS];
static unsigned trend_count;
static const uint32_t trend_colors[] = {BLUE, GREEN, ORANGE};
typedef struct { double counts[5]; double success_rate; } health_point_t;
static health_point_t health_points[TREND_POINTS];
static unsigned health_count;
static double health_axis_max;
static const uint32_t health_colors[] = {0x12B981, RED, BLUE, ORANGE, 0x94A3B8};
static char counter_pattern[COUNTER_COUNT][20];
static lv_obj_t *counter_digits[COUNTER_COUNT][20];
static int small_digit_width;
static lv_obj_t *table_outer[TABLE_COUNT], *table_horizontal[TABLE_COUNT];
static cJSON *shown[CPR_RESOURCE_COUNT];
static unsigned table_version[TABLE_COUNT], summary_version, active_page, interval = 5, refresh_index = 1;
static const unsigned refresh_seconds[] = {10, 30, 60, 120, 300, 600};
static lv_point_t scroll_positions[CPR_RESOURCE_COUNT];
static bool automatic_pages, was_on = true, overview_refresh_started, overview_waiting;
static uint32_t page_started_ms;
static const char *page_names[] = {"总览", "健康", "模型", "账号", "密钥", "错误", "请求"};
static const cpr_resource_t page_resources[] = {CPR_SUMMARY, CPR_SUMMARY, CPR_MODEL, CPR_ACCOUNT,
                                               CPR_KEY, CPR_ERROR, CPR_REQUESTS};

static uint32_t milliseconds(void) { return esp_timer_get_time() / 1000; }
static cJSON *field(cJSON *value, const char *name) { return cJSON_GetObjectItemCaseSensitive(value, name); }
static const char *text(cJSON *value, const char *name)
{
    cJSON *item = field(value, name);
    return cJSON_IsString(item) && item->valuestring[0] ? item->valuestring : "--";
}
static double number(cJSON *value, const char *name)
{
    cJSON *item = field(value, name);
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}
static void formatted_number(char *out, size_t size, double value)
{
    if (!isfinite(value)) snprintf(out, size, "--");
    else if (fabs(value) >= 1000000) snprintf(out, size, "%.1fM", value / 1000000);
    else if (fabs(value) >= 10000) snprintf(out, size, "%.1fK", value / 1000);
    else snprintf(out, size, "%.0f", value);
}
static void formatted_integer(char *out, size_t size, double value)
{
    if (isfinite(value)) snprintf(out, size, "%.0f", value);
    else snprintf(out, size, "--");
}
static void duration(char *out, size_t size, double value)
{
    if (!isfinite(value)) snprintf(out, size, "--");
    else snprintf(out, size, "%.2fs", value / 1000);
}
static void set_text(lv_obj_t *label, const char *value)
{
    if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}
static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int width, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, &font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_label_set_text(object, value);
    return object;
}
static lv_obj_t *button(lv_obj_t *parent, const char *value, int x, int y, int width,
                        lv_event_cb_t callback, void *data)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 28);
    lv_obj_set_style_bg_color(object, lv_color_hex(ELEVATED), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 5, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *caption = label(object, value, 0, 0, width, INK);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
    if (callback) lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, data);
    return object;
}
static cpr_resource_t resource(void)
{
    return page_resources[active_page];
}
static void save_position(void)
{
    if (active_page < 2) return;
    unsigned table = active_page - 2;
    if (table_horizontal[table]) {
        scroll_positions[resource()].x = lv_obj_get_scroll_x(table_horizontal[table]);
        scroll_positions[resource()].y = lv_obj_get_scroll_y(table_outer[table]);
    }
}
void cpr_ui_select_page(unsigned page)
{
    save_position();
    active_page = page % PAGE_COUNT;
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        if (i == active_page) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    char value[32];
    snprintf(value, sizeof(value), "%s%u/%u", page_names[active_page], active_page + 1, PAGE_COUNT);
    set_text(page_text, value);
    page_started_ms = milliseconds();
    overview_refresh_started = overview_waiting = false;
}
static void next_page(lv_event_t *event)
{
    intptr_t direction = (intptr_t)lv_event_get_user_data(event);
    cpr_ui_select_page((active_page + PAGE_COUNT + direction) % PAGE_COUNT);
}
static void auto_changed(lv_event_t *event)
{
    automatic_pages = lv_obj_has_state(auto_box, LV_STATE_CHECKED);
    cpr_api_set_auto_pages(automatic_pages);
    cpr_api_lock();
    notice_version = cpr_api_notice()->version;
    cpr_api_unlock();
    if (automatic_pages) {
        lv_obj_remove_flag(interval_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(refresh_interval_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(interval_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(refresh_interval_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_x(auto_box, automatic_pages ? 161 : 219);
    overview_refresh_started = overview_waiting = false;
    page_started_ms = milliseconds();
}
static void set_interval_text(lv_obj_t *caption, unsigned seconds)
{
    char value[16];
    snprintf(value, sizeof(value), "%u秒", seconds);
    lv_label_set_text(caption, value);
    lv_obj_center(caption);
}
static void interval_changed(lv_event_t *event)
{
    interval = interval == 10 ? 3 : interval + 1;
    set_interval_text(interval_text, interval);
    page_started_ms = milliseconds();
}
static void refresh_interval_changed(lv_event_t *event)
{
    refresh_index = (refresh_index + 1) % 6;
    cpr_api_set_refresh_seconds(refresh_seconds[refresh_index]);
    set_interval_text(refresh_interval_text, refresh_seconds[refresh_index]);
}
static void refresh_clicked(lv_event_t *event)
{
    if (!automatic_pages) cpr_api_refresh_all();
}

static void update_counter(unsigned index, const char *value)
{
    bool primary = index < COUNTER_TOTAL_REQUESTS;
    const lv_font_t *number_font = primary ? &cpr_misans_32 : &font;
    int digit_width = primary ? 23 : small_digit_width;
    uint32_t color = primary ? INK : MUTED;
    char pattern[20];
    size_t length = strlen(value);
    if (length >= sizeof(pattern)) length = sizeof(pattern) - 1;
    for (size_t i = 0; i < length; ++i) pattern[i] = value[i] >= '0' && value[i] <= '9' ? 'd' : value[i];
    pattern[length] = 0;
    bool rebuild = strcmp(pattern, counter_pattern[index]) != 0;
    if (rebuild) {
        lv_obj_clean(metric_values[index]);
        strcpy(counter_pattern[index], pattern);
        int x = 0;
        for (size_t i = 0; i < length; ++i) {
            if (pattern[i] == 'd') {
                lv_obj_t *roller = lv_roller_create(metric_values[index]);
                lv_obj_remove_style_all(roller);
                lv_obj_set_style_text_font(roller, number_font, LV_PART_MAIN);
                lv_obj_set_style_text_font(roller, number_font, LV_PART_SELECTED);
                lv_obj_set_style_text_color(roller, lv_color_hex(color), LV_PART_MAIN);
                lv_obj_set_style_text_color(roller, lv_color_hex(color), LV_PART_SELECTED);
                lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_anim_duration(roller, 300, 0);
                lv_roller_set_options(roller, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9", LV_ROLLER_MODE_INFINITE);
                lv_roller_set_visible_row_count(roller, 1);
                lv_obj_set_size(roller, digit_width, primary ? 38 : font.line_height);
                lv_obj_set_pos(roller, x, 0);
                lv_obj_remove_flag(roller, LV_OBJ_FLAG_CLICKABLE);
                counter_digits[index][i] = roller;
                x += digit_width;
            } else {
                size_t start = i;
                while (i + 1 < length && pattern[i + 1] != 'd') ++i;
                char literal[20];
                memcpy(literal, value + start, i - start + 1);
                literal[i - start + 1] = 0;
                lv_point_t size;
                lv_text_get_size(&size, literal, number_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                int advance = size.x;
                if (primary) {
                    advance = 0;
                    for (size_t j = start; j <= i; ++j) advance += value[j] == '.' ? 11 : 31;
                }
                lv_obj_t *part = label(metric_values[index], literal, x, 0, advance, color);
                lv_obj_set_style_text_font(part, number_font, 0);
                x += advance;
                for (size_t j = start; j <= i; ++j) counter_digits[index][j] = NULL;
            }
        }
    }
    for (size_t i = 0; i < length; ++i) {
        lv_obj_t *digit = counter_digits[index][i];
        if (digit && (rebuild || lv_roller_get_selected(digit) != value[i] - '0')) {
            lv_roller_set_selected(digit, value[i] - '0', rebuild ? LV_ANIM_OFF : LV_ANIM_ON);
        }
    }
}
static void draw_trend(lv_event_t *event)
{
    if (trend_count < 2) return;
    unsigned index = (uintptr_t)lv_event_get_user_data(event);
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    int width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_color_t color = lv_color_hex(trend_colors[index]);
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = color;
    fill.bg_grad.dir = LV_GRAD_DIR_VER;
    fill.bg_grad.stops_count = 2;
    fill.bg_grad.stops[0].color = fill.bg_grad.stops[1].color = color;
    fill.bg_grad.stops[1].frac = 255;
    fill.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    for (int x = 0; x < width; ++x) {
        int position = x * (trend_count - 1), segment = position / (width - 1);
        if (segment >= trend_count - 1) segment = trend_count - 2;
        int a = trend_values[index][segment], b = trend_values[index][segment + 1];
        if (a < 0 || b < 0) continue;
        int value = a + (b - a) * (position - segment * (width - 1)) / (width - 1);
        int y = height - 3 - value * (height - 12) / 1000;
        fill.bg_grad.stops[0].opa = 44 * (height - y) / height;
        lv_area_t stripe = {bounds.x1 + x, bounds.y1 + y, bounds.x1 + x, bounds.y2};
        lv_draw_rect(layer, &fill, &stripe);
    }
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 1;
    line.opa = LV_OPA_50;
    for (unsigned i = 1; i < trend_count; ++i) {
        int a = trend_values[index][i - 1], b = trend_values[index][i];
        if (a < 0 || b < 0) continue;
        line.p1.x = bounds.x1 + (i - 1) * (width - 1) / (trend_count - 1);
        line.p2.x = bounds.x1 + i * (width - 1) / (trend_count - 1);
        line.p1.y = bounds.y2 - 2 - a * (height - 12) / 1000;
        line.p2.y = bounds.y2 - 2 - b * (height - 12) / 1000;
        lv_draw_line(layer, &line);
    }
}

static void render_overview(cJSON *data)
{
    cJSON *cards = field(data, "cards");
    cJSON *traffic = field(cards, "traffic"), *tokens = field(cards, "tokens"), *cache = field(cards, "cache");
    char requests[24];
    formatted_integer(requests, sizeof(requests), number(traffic, "todayRequestsValue"));
    update_counter(COUNTER_REQUESTS, requests);
    update_counter(COUNTER_TOKENS, text(tokens, "todayTokens"));
    update_counter(COUNTER_HIT_RATE, text(cache, "todayHitRate"));
    update_counter(COUNTER_TOTAL_REQUESTS, text(traffic, "totalRequests"));
    update_counter(COUNTER_FIRST_TOKEN, text(cache, "averageFirstTokenLatencyMs"));
    update_counter(COUNTER_TOTAL_TOKENS, text(tokens, "totalTokens"));
    char value[160];
    const char *amount = text(tokens, "totalBillingAmountUsd");
    char cost[32];
    if (strcmp(amount, "--") == 0) snprintf(cost, sizeof(cost), "--");
    else snprintf(cost, sizeof(cost), "$%.2f", strtod(amount + (amount[0] == '$'), NULL));
    update_counter(COUNTER_COST, cost);
    update_counter(COUNTER_TOTAL_HIT_RATE, text(cache, "totalHitRate"));
    update_counter(COUNTER_CACHED_TOKENS, text(cache, "totalCachedInputTokens"));
    cJSON *capacity = field(data, "capacityInfo");
    char used[24], total[24];
    formatted_number(used, sizeof(used), number(capacity, "usedSlots"));
    double total_slots = number(capacity, "totalSlots"), used_slots = number(capacity, "usedSlots");
    if (isfinite(total_slots)) formatted_number(total, sizeof(total), total_slots);
    else snprintf(total, sizeof(total), "无限制");
    snprintf(value, sizeof(value), "槽位占用  %s / %s", used, total);
    set_text(slot_text, value);
    lv_bar_set_value(slot_bar, isfinite(total_slots) && total_slots > 0 && isfinite(used_slots) ? used_slots * 100 / total_slots : 0, LV_ANIM_OFF);
    if (!isfinite(total_slots) || !isfinite(used_slots)) lv_obj_add_flag(slot_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(slot_bar, LV_OBJ_FLAG_HIDDEN);
    cJSON *points = field(field(data, "trend"), "points");
    trend_count = 0;
    cJSON *point;
    cJSON_ArrayForEach(point, points) {
        if (trend_count == TREND_POINTS || strncmp(text(point, "bucket"), text(data, "asOf"), 19) > 0) break;
        ++trend_count;
    }
    for (unsigned chart = 0; chart < 3; ++chart) {
        const char *key = chart == 0 ? "requestsValue" : chart == 1 ? "tokensValue" : "cacheHitRateValue";
        double maximum = 1;
        for (unsigned i = 0; i < trend_count; ++i) {
            double n = number(cJSON_GetArrayItem(points, i), key);
            if (isfinite(n) && n > maximum) maximum = n;
        }
        for (unsigned i = 0; i < trend_count; ++i) {
            double n = number(cJSON_GetArrayItem(points, i), key);
            trend_values[chart][i] = isfinite(n) ? n / maximum * 1000 : -1;
        }
        lv_obj_invalidate(charts[chart]);
    }
}

static void draw_health(lv_event_t *event)
{
    if (!health_count) return;
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    int width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;
    line.color = lv_color_hex(BORDER);
    line.opa = LV_OPA_40;
    for (unsigned i = 0; i < 3; ++i) {
        line.p1 = (lv_point_precise_t){bounds.x1, bounds.y1 + i * (height - 1) / 2};
        line.p2 = (lv_point_precise_t){bounds.x2, line.p1.y};
        lv_draw_line(layer, &line);
    }
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_opa = LV_OPA_70;
    int bar_width = width / health_count - 1;
    if (bar_width < 1) bar_width = 1;
    if (bar_width > 24) bar_width = 24;
    for (unsigned i = 0; i < health_count; ++i) {
        double stacked = 0;
        int center = bounds.x1 + (2 * i + 1) * width / (2 * health_count);
        for (unsigned category = 0; category < 5; ++category) {
            double count = health_points[i].counts[category];
            if (!isfinite(count)) break;
            int bottom = bounds.y2 - (int)round(stacked / health_axis_max * (height - 1));
            stacked += count;
            int top = bounds.y2 - (int)round(stacked / health_axis_max * (height - 1));
            if (top < bottom) {
                bar.bg_color = lv_color_hex(health_colors[category]);
                lv_area_t area = {center - bar_width / 2, top, center - bar_width / 2 + bar_width - 1, bottom - 1};
                lv_draw_rect(layer, &bar, &area);
            }
        }
    }
    line.color = lv_color_hex(health_colors[0]);
    line.opa = LV_OPA_COVER;
    line.width = 2;
    for (unsigned i = 1; i < health_count; ++i) {
        double a = health_points[i - 1].success_rate, b = health_points[i].success_rate;
        if (!isfinite(a) || !isfinite(b)) continue;
        line.p1.x = bounds.x1 + (2 * i - 1) * width / (2 * health_count);
        line.p2.x = bounds.x1 + (2 * i + 1) * width / (2 * health_count);
        line.p1.y = bounds.y2 - (int)round(a * (height - 1));
        line.p2.y = bounds.y2 - (int)round(b * (height - 1));
        lv_draw_line(layer, &line);
    }
}
static void render_health(cJSON *data)
{
    lv_obj_clean(pages[1]);
    cJSON *health = field(data, "healthTimeline");
    double success = number(health, "successRequests"), failed = number(health, "failedRequests");
    double cancelled = number(health, "cancelledRequests"), incomplete = number(health, "incompleteRequests");
    double caller = number(health, "callerErrorRequests");
    double total = success + failed + cancelled + incomplete + caller;
    double completed = total > 0 ? (success + failed + caller) / total * 100 : NAN;
    char value[96], completion[24], abnormal[24];
    if (isfinite(completed)) snprintf(completion, sizeof(completion), "%.1f%%", completed);
    else snprintf(completion, sizeof(completion), "--");
    formatted_integer(abnormal, sizeof(abnormal), cancelled + incomplete);
    snprintf(value, sizeof(value), "服务成功率 %s", text(health, "reliabilityDisplay"));
    label(pages[1], value, 10, 0, 210, GREEN);
    snprintf(value, sizeof(value), "完成率 %s", completion);
    label(pages[1], value, 228, 0, 190, INK);
    snprintf(value, sizeof(value), "非正常结束 %s", abnormal);
    label(pages[1], value, 440, 0, 194, ORANGE);
    const char *names[] = {"成功", "服务失败", "取消", "未完成", "调用方错误", "服务成功率"};
    const int positions[] = {10, 82, 195, 270, 359, 503};
    for (unsigned i = 0; i < 6; ++i) {
        box(pages[1], positions[i], 29, 6, 6, i == 5 ? health_colors[0] : health_colors[i]);
        label(pages[1], names[i], positions[i] + 10, 23, i == 4 ? 115 : 120, MUTED);
    }
    cJSON *points = field(health, "points");
    const char *keys[] = {"successRequests", "failedRequests", "cancelledRequests", "incompleteRequests", "callerErrorRequests"};
    health_count = 0;
    double maximum = 0;
    cJSON *point;
    cJSON_ArrayForEach(point, points) {
        if (health_count == TREND_POINTS || strncmp(text(point, "bucketStart"), text(data, "asOf"), 19) > 0) break;
        health_point_t *entry = &health_points[health_count++];
        double observed = 0;
        for (unsigned i = 0; i < 5; ++i) { entry->counts[i] = number(point, keys[i]); observed += entry->counts[i]; }
        if (observed > maximum) maximum = observed;
        double eligible = entry->counts[0] + entry->counts[1] + entry->counts[3];
        entry->success_rate = eligible > 0 ? entry->counts[0] / eligible : observed == 0 ? 0 : NAN;
    }
    if (!health_count || maximum == 0) {
        health_count = 0;
        label(pages[1], data ? "今日暂无请求健康数据" : "正在读取今日健康数据…", 10, 75, 610, MUTED);
        return;
    }
    health_axis_max = maximum > 10 ? ceil(maximum / 10) * 10 : ceil(maximum);
    lv_obj_t *plot = box(pages[1], 36, 48, 568, 70, BG);
    lv_obj_add_event_cb(plot, draw_health, LV_EVENT_DRAW_MAIN, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(value, sizeof(value), "%.0f", health_axis_max * (2 - i) / 2);
        lv_obj_t *count = label(pages[1], value, 0, 41 + i * 35, 31, MUTED);
        lv_obj_set_style_text_font(count, &cpr_misans_12, 0);
        lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
        snprintf(value, sizeof(value), "%u%%", (2 - i) * 50);
        lv_obj_t *rate = label(pages[1], value, 608, 41 + i * 35, 32, MUTED);
        lv_obj_set_style_text_font(rate, &cpr_misans_12, 0);
    }
    unsigned ticks = health_count < 5 ? health_count : 5;
    for (unsigned i = 0; i < ticks; ++i) {
        unsigned index = ticks > 1 ? i * (health_count - 1) / (ticks - 1) : 0;
        int x = ticks > 1 ? 11 + i * 568 / (ticks - 1) : 36;
        lv_text_align_t align = LV_TEXT_ALIGN_CENTER;
        if (i == 0) { x = 36; align = LV_TEXT_ALIGN_LEFT; }
        else if (i == ticks - 1) { x = 554; align = LV_TEXT_ALIGN_RIGHT; }
        lv_obj_t *time = label(pages[1], text(cJSON_GetArrayItem(points, index), "time"), x, 119, 50, MUTED);
        lv_obj_set_style_text_align(time, align, 0);
    }
}

static const char *record_time(cJSON *row)
{
    const char *created = text(row, "createdAtDisplay");
    return strlen(created) > 11 && created[10] == ' ' ? created + 11 : created;
}
static const char *record_account(cJSON *row)
{
    const char *account = text(row, "accountEmail");
    if (strcmp(account, "--") == 0) account = text(row, "accountName");
    if (strcmp(account, "--") == 0) account = text(field(row, "metadata"), "accountLabel");
    if (strcmp(account, "--") == 0) account = text(row, "accountId");
    return account;
}
static const char *error_code(cJSON *row)
{
    return text(row, "errorCode");
}
static const char *error_description(cJSON *row)
{
    const char *message = text(row, "errorMessage");
    return strcmp(message, "--") == 0 ? "" : message;
}
static const char *request_outcome(cJSON *row)
{
    const char *outcome = text(row, "logicalOutcome");
    if (strcmp(outcome, "succeeded") == 0) return "成功";
    if (strcmp(outcome, "failed") == 0) return "失败";
    if (strcmp(outcome, "cancelled") == 0) return "取消";
    if (strcmp(outcome, "incomplete") == 0) return "未完成";
    if (strcmp(outcome, "running") == 0) return "进行中";
    return outcome;
}
static uint32_t outcome_color(cJSON *row)
{
    const char *outcome = text(row, "logicalOutcome");
    if (strcmp(outcome, "succeeded") == 0) return GREEN;
    if (strcmp(outcome, "failed") == 0) return RED;
    if (strcmp(outcome, "cancelled") == 0) return BLUE;
    if (strcmp(outcome, "incomplete") == 0) return ORANGE;
    return MUTED;
}
static const char *cell_text(cJSON *row, cpr_resource_t res, unsigned column, char *out, size_t size)
{
    if (res == CPR_REQUESTS) {
        switch (column) {
        case 0: return record_time(row);
        case 1: return request_outcome(row);
        case 2: return text(row, "clientApiKeyName");
        case 3: return record_account(row);
        case 4: {
            const char *model = text(row, "requestedModel");
            if (strcmp(model, "--") == 0) model = text(row, "model");
            return model;
        }
        case 5: {
            cJSON *tokens = field(row, "tokenDetails");
            double input = number(tokens, "inputTokens"), cached = number(tokens, "cachedInputTokens"), written = number(tokens, "cacheWriteInputTokens");
            char a[24], b[24], c[24];
            formatted_number(a, sizeof(a), isfinite(input) ? fmax(0, input - (isfinite(cached) ? cached : 0) - (isfinite(written) ? written : 0)) : NAN);
            formatted_number(b, sizeof(b), number(tokens, "outputTokens"));
            formatted_number(c, sizeof(c), cached);
            snprintf(out, size, "入 %s 出 %s\n缓存 %s", a, b, c); break;
        }
        case 6: {
            char a[24], b[24]; duration(a, sizeof(a), number(row, "firstTokenLatencyMs")); duration(b, sizeof(b), number(row, "latencyMs"));
            snprintf(out, size, "首字 %s\n总计 %s", a, b); break;
        }
        case 7: {
            double output = number(field(row, "tokenDetails"), "outputTokens");
            double upstream = number(field(row, "latencyDetails"), "upstreamResponseMs");
            if (isfinite(output) && output > 0 && isfinite(upstream) && upstream > 0) {
                snprintf(out, size, "%.0f tok/s", output * 1000 / upstream);
            } else snprintf(out, size, "--");
            break;
        }
        case 8: return text(row, "clientIp");
        case 9: return error_code(row);
        default: out[0] = 0;
        }
        return out;
    }
    if (column == 0) return text(row, "name");
    char a[32], b[32];
    switch (column) {
    case 1:
        formatted_integer(a, sizeof(a), number(row, "requestCount"));
        snprintf(out, size, "%s\n占比 %.1f%%", a, number(row, "requestShare") * 100); break;
    case 2:
        formatted_integer(a, sizeof(a), number(row, "errorCount"));
        snprintf(out, size, "%s\n失败率 %.1f%%", a, number(row, "errorRate") * 100); break;
    case 3:
        duration(a, sizeof(a), number(row, "firstTokenP95Ms")); duration(b, sizeof(b), number(row, "latencyP95Ms"));
        snprintf(out, size, "首字P95 %s\n总P95 %s", a, b); break;
    case 4:
        formatted_integer(a, sizeof(a), number(row, "retryCount"));
        snprintf(out, size, "%s\n重试率 %.1f%%", a, number(row, "retryRate") * 100); break;
    default: return text(row, "estimatedCost");
    }
    return out;
}
static int text_width(const char *value)
{
    lv_point_t size;
    lv_text_get_size(&size, value, &font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void render_table(cpr_resource_t res, const cpr_snapshot_t *snapshot)
{
    unsigned table = active_page - 2;
    save_position();
    int old_first = scroll_positions[res].y > 28 ? (scroll_positions[res].y - 28) / ROW_HEIGHT : 0;
    char anchor[128] = "";
    cJSON *old_row = cJSON_GetArrayItem(field(shown[res], "items"), old_first);
    if (old_row && scroll_positions[res].y >= 28) {
        snprintf(anchor, sizeof(anchor), "%s", text(old_row, res == CPR_REQUESTS ? "id" : "key"));
    }
    cJSON_Delete(shown[res]);
    shown[res] = cJSON_Duplicate(snapshot->data, true);
    if (!snapshot->data) scroll_positions[res] = (lv_point_t){0};
    lv_obj_clean(table_outer[table]);
    cJSON *items = field(shown[res], "items");
    int count = cJSON_GetArraySize(items);
    const char *request_headers[] = {"时间", "结果", "密钥名称", "账号", "模型", "TOKEN", "延迟", "速率", "IP", "错误"};
    const char *diagnostic_headers[] = {"名称", "请求", "失败", "性能", "重试", "费用(USD)"};
    const char *account_headers[] = {"账号", "请求", "失败", "性能", "重试", "费用(USD)"};
    int widths[10];
    int padding = text_width("汉");
    bool account = res == CPR_ACCOUNT, requests = res == CPR_REQUESTS;
    unsigned columns = requests ? 10 : 6;
    const char **headers = requests ? request_headers : account ? account_headers : diagnostic_headers;
    for (unsigned column = 0; column < columns; ++column) {
        int widest = text_width(headers[column]);
        cJSON *row;
        cJSON_ArrayForEach(row, items) {
            char buffer[160];
            int needed = text_width(cell_text(row, res, column, buffer, sizeof(buffer)));
            if (account && column == 0 && strcmp(text(row, "accountNotes"), "--") != 0) needed += text_width(text(row, "accountNotes")) + 8;
            if (requests && column == 9) {
                int description = text_width(error_description(row));
                if (description > needed) needed = description;
            }
            if (needed > widest) widest = needed;
        }
        if (((account && column == 0) || (requests && column == 3)) && widest > padding * 5) widest = padding * 5;
        widths[column] = widest + padding * 2;
    }
    int total_width = 0;
    for (unsigned i = 0; i < columns; ++i) total_width += widths[i];
    int height = 28 + count * ROW_HEIGHT;
    if (height < 72) height = 72;
    table_horizontal[table] = box(table_outer[table], 0, 0, 640, height, SURFACE);
    lv_obj_add_flag(table_horizontal[table], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(table_horizontal[table], LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(table_horizontal[table], LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *grid = box(table_horizontal[table], 0, 0, total_width, height, SURFACE);
    int x = 0;
    for (unsigned column = 0; column < columns; ++column) {
        label(grid, headers[column], x + padding, 4, widths[column] - padding * 2, MUTED);
        x += widths[column];
    }
    for (int i = 0; i < count; ++i) {
        cJSON *row = cJSON_GetArrayItem(items, i);
        if (anchor[0] && strcmp(anchor, text(row, requests ? "id" : "key")) == 0) {
            scroll_positions[res].y = 28 + i * ROW_HEIGHT + (scroll_positions[res].y > 28 ? (scroll_positions[res].y - 28) % ROW_HEIGHT : 0);
        }
        lv_obj_t *background = box(grid, 0, 28 + i * ROW_HEIGHT, total_width, ROW_HEIGHT, i % 2 ? ELEVATED : SURFACE);
        lv_obj_add_flag(background, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        x = 0;
        for (unsigned column = 0; column < columns; ++column) {
            char buffer[160];
            const char *value = cell_text(row, res, column, buffer, sizeof(buffer));
            uint32_t color = requests && column == 1 ? outcome_color(row) :
                             requests && column == 9 ? (strcmp(value, "--") == 0 ? MUTED : RED) : INK;
            lv_obj_t *cell = label(background, value, x + padding, 4, widths[column] - padding * 2, color);
            lv_label_set_long_mode(cell, LV_LABEL_LONG_CLIP);
            if ((account && column == 0) || (requests && column == 3)) {
                lv_obj_set_height(cell, font.line_height);
                lv_label_set_long_mode(cell, LV_LABEL_LONG_DOT);
            }
            if (account && column == 0) {
                const char *notes = text(row, "accountNotes");
                if (strcmp(notes, "--") != 0) {
                    int name_width = text_width(value);
                    int available = widths[column] - padding * 2;
                    int note_width = text_width(notes);
                    if (note_width > available / 2) note_width = available / 2;
                    if (name_width > available - note_width - 8) name_width = available - note_width - 8;
                    lv_obj_set_width(cell, name_width);
                    lv_obj_t *note = label(background, notes, x + padding + name_width + 8, 4,
                                          widths[column] - padding * 2 - name_width - 8, MUTED);
                    lv_obj_set_height(note, font.line_height);
                }
            } else if (requests && column == 9) {
                lv_obj_set_height(cell, font.line_height);
                lv_obj_t *description = label(background, error_description(row), x + padding, 24,
                                              widths[column] - padding * 2, MUTED);
                lv_label_set_long_mode(description, LV_LABEL_LONG_CLIP);
            }
            x += widths[column];
        }
    }
    if (!count) label(grid, snapshot->data ? "今日暂无记录" : snapshot->error[0] ? "今日数据尚未读取成功" : "正在读取今日数据…", 12, 35, 580, MUTED);
    lv_obj_update_layout(table_outer[table]);
    lv_obj_scroll_to_y(table_outer[table], scroll_positions[res].y, LV_ANIM_OFF);
    lv_obj_scroll_to_x(table_horizontal[table], scroll_positions[res].x, LV_ANIM_OFF);
    table_version[table] = snapshot->version;
}

void cpr_ui_init(void)
{
    font = cpr_misans_14;
    font.fallback = &lv_font_montserrat_14;
    for (char digit = '0'; digit <= '9'; ++digit) {
        int width = lv_font_get_glyph_width(&font, digit, 0);
        if (width > small_digit_width) small_digit_width = width;
    }
    lv_display_t *display = lv_display_get_default();
    lv_display_set_theme(display, lv_theme_default_init(display, lv_color_hex(BLUE),
                                                       lv_color_hex(GREEN), true, &font));
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_text_font(screen, &font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) pages[i] = box(screen, 0, 34, 640, 138, BG);
    const char *names[] = {"今日请求", "今日 Token", "缓存命中"};
    const char *details[][2] = {{"累计", "首字"}, {"累计", "费用"}, {"累计命中", "缓存"}};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *card = box(pages[0], 6 + i * 212, 3, 204, 108, SURFACE);
        lv_obj_set_style_radius(card, 9, 0);
        lv_obj_set_style_clip_corner(card, true, 0);
        charts[i] = box(card, 0, 0, 204, 108, SURFACE);
        lv_obj_set_style_bg_opa(charts[i], LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(charts[i], draw_trend, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)i);
        label(card, names[i], 10, 5, 170, MUTED);
        metric_values[i] = box(card, 8, 25, 192, 40, SURFACE);
        lv_obj_set_style_bg_opa(metric_values[i], LV_OPA_TRANSP, 0);
        update_counter(i, "--");
        for (unsigned row = 0; row < 2; ++row) {
            int prefix_width = text_width(details[i][row]);
            int y = 67 + row * font.line_height, x = 10 + prefix_width + 4;
            label(card, details[i][row], 10, y, prefix_width, MUTED);
            unsigned counter = COUNTER_TOTAL_REQUESTS + i * 2 + row;
            metric_values[counter] = box(card, x, y, 204 - x, font.line_height, SURFACE);
            lv_obj_set_style_bg_opa(metric_values[counter], LV_OPA_TRANSP, 0);
            update_counter(counter, "--");
        }
    }
    slot_text = label(pages[0], "槽位占用 -- / --", 12, 117, 242, INK);
    slot_bar = lv_bar_create(pages[0]);
    lv_obj_set_pos(slot_bar, 254, 122); lv_obj_set_size(slot_bar, 372, 6);
    lv_obj_set_style_bg_color(slot_bar, lv_color_hex(BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot_bar, lv_color_hex(BLUE), LV_PART_INDICATOR);
    label(pages[1], "健康时间线正在读取…", 12, 20, 600, MUTED);
    for (unsigned i = 0; i < TABLE_COUNT; ++i) {
        table_outer[i] = box(pages[i + 2], 0, 0, 640, 138, SURFACE);
        lv_obj_add_flag(table_outer[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(table_outer[i], LV_DIR_VER);
        label(table_outer[i], "正在读取今日数据…", 12, 16, 590, MUTED);
    }
    lv_obj_t *bar = box(lv_layer_top(), 0, 0, 640, 32, SURFACE);
    connection = label(bar, "CPR 连接中", 6, 7, 78, ORANGE);
    refresh_control = button(bar, "刷新中", 85, 2, 72, refresh_clicked, NULL);
    refresh_text = lv_obj_get_child(refresh_control, 0);
    refresh_interval_button = button(bar, "30秒", 161, 2, 54, refresh_interval_changed, NULL);
    refresh_interval_text = lv_obj_get_child(refresh_interval_button, 0);
    auto_box = lv_checkbox_create(bar);
    lv_checkbox_set_text(auto_box, "自动翻页");
    lv_obj_set_style_text_font(auto_box, &font, 0);
    lv_obj_set_style_text_color(auto_box, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(auto_box, lv_color_hex(ELEVATED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(auto_box, lv_color_hex(BLUE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_pos(auto_box, 219, 4);
    lv_obj_add_event_cb(auto_box, auto_changed, LV_EVENT_VALUE_CHANGED, NULL);
    interval_button = button(bar, "5秒", 253, 2, 43, interval_changed, NULL);
    interval_text = lv_obj_get_child(interval_button, 0);
    lv_obj_add_flag(interval_button, LV_OBJ_FLAG_HIDDEN);
    button(bar, LV_SYMBOL_LEFT, 309, 2, 58, next_page, (void *)(intptr_t)-1);
    lv_obj_t *page_button = button(bar, "总览1/7", 371, 2, 62, NULL, NULL);
    page_text = lv_obj_get_child(page_button, 0);
    button(bar, LV_SYMBOL_RIGHT, 437, 2, 58, next_page, (void *)(intptr_t)1);
    battery_text = label(bar, "BAT --", 544, 0, 90, MUTED);
    lv_obj_set_style_text_font(battery_text, &cpr_misans_12, 0);
    lv_obj_set_style_text_align(battery_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(battery_text, 0, 0);
    alert = label(lv_layer_top(), "", 8, 151, 622, RED);
    lv_obj_set_style_bg_color(alert, lv_color_hex(0x30251E), 0);
    lv_obj_set_style_bg_opa(alert, LV_OPA_COVER, 0);
    lv_obj_add_flag(alert, LV_OBJ_FLAG_HIDDEN);
    shutdown_panel = box(lv_layer_top(), 0, 0, 640, 172, BG);
    shutdown_text = label(shutdown_panel, "", 70, 40, 500, INK);
    lv_obj_set_style_text_align(shutdown_text, LV_TEXT_ALIGN_CENTER, 0);
    shutdown_bar = lv_bar_create(shutdown_panel);
    lv_obj_set_pos(shutdown_bar, 70, 94); lv_obj_set_size(shutdown_bar, 500, 12);
    lv_obj_set_style_bg_color(shutdown_bar, lv_color_hex(BLUE), LV_PART_INDICATOR);
    lv_obj_add_flag(shutdown_panel, LV_OBJ_FLAG_HIDDEN);
    error_toast = box(lv_layer_top(), 60, 40, 520, 88, 0x30202A);
    lv_obj_set_style_radius(error_toast, 8, 0);
    lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_CLICKABLE);
    error_title = label(error_toast, "", 12, 6, 496, RED);
    error_message = label(error_toast, "", 12, 29, 496, INK);
    lv_obj_set_height(error_message, 54);
    lv_label_set_long_mode(error_message, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    page_started_ms = milliseconds();
    cpr_ui_select_page(0);
}

void cpr_ui_update(const wifi_link_status_t *network, const board_battery_status_t *battery, int shutdown)
{
    uint32_t now = milliseconds();
    if (board_display_is_touched()) page_started_ms = now;
    bool on = board_display_is_on();
    if (on != was_on) cpr_api_set_display_on(on);
    if (on && !was_on) page_started_ms = now;
    was_on = on;
    if (!on || shutdown >= 0) page_started_ms = now;
    if (shutdown >= 0) {
        lv_obj_remove_flag(shutdown_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(shutdown_panel);
        set_text(shutdown_text, shutdown >= 100 ? "请松开电源键关机" : "继续按住电源键，松手取消关机");
        lv_bar_set_value(shutdown_bar, shutdown, LV_ANIM_OFF);
        return;
    }
    lv_obj_add_flag(shutdown_panel, LV_OBJ_FLAG_HIDDEN);
    char value[160];
    if (battery->available) snprintf(value, sizeof(value), "~%d%%\n%.2fV", battery->percent, battery->millivolts / 1000.0);
    else snprintf(value, sizeof(value), "BAT --");
    set_text(battery_text, value);
    cpr_resource_t res = resource();
    cpr_api_lock();
    const cpr_snapshot_t *summary = cpr_api_snapshot(CPR_SUMMARY);
    const cpr_snapshot_t *snapshot = cpr_api_snapshot(res);
    cpr_refresh_state_t cycle = *cpr_api_refresh_state();
    const cpr_notice_t *notice = cpr_api_notice();
    if (automatic_pages && on && notice->version != notice_version) {
        notice_version = notice->version;
        snprintf(value, sizeof(value), "新错误 (%u)  %s", notice->count, notice->code);
        set_text(error_title, value);
        set_text(error_message, notice->message);
        notice_shown_ms = now;
        lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(error_toast);
    }
    if (!automatic_pages || !on || (notice_shown_ms && now - notice_shown_ms >= 5000)) {
        lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    }
    const char *state = network->state == WIFI_LINK_NEEDS_CONFIG ? "请配置WiFi" :
                        network->state != WIFI_LINK_CONNECTED ? "WiFi未连接" :
                        time(NULL) < 1735689600 ? "正在校时" :
                        cpr_api_auth_denied() ? "鉴权失败" :
                        snapshot->error[0] ? "CPR 异常" : summary->data ? "CPR 在线" : "CPR 连接中";
    set_text(connection, state);
    lv_obj_set_style_text_color(connection, lv_color_hex(snapshot->error[0] ? RED : summary->data && network->state == WIFI_LINK_CONNECTED ? GREEN : ORANGE), 0);
    if (cycle.refreshing) snprintf(value, sizeof(value), "刷新%u/7", cycle.completed);
    else if (automatic_pages) snprintf(value, sizeof(value), "每轮刷新");
    else {
        int32_t remaining = cycle.next_ms - now;
        snprintf(value, sizeof(value), "刷新%lu秒", (unsigned long)(remaining > 0 ? (remaining + 999) / 1000 : 0));
    }
    set_text(refresh_text, value);
    if (cycle.refreshing || automatic_pages || cpr_api_auth_denied()) lv_obj_add_state(refresh_control, LV_STATE_DISABLED);
    else lv_obj_remove_state(refresh_control, LV_STATE_DISABLED);
    char stale[160];
    if (snapshot->error[0] && snapshot->data) {
        snprintf(stale, sizeof(stale), "%s · 旧数据 %lu秒前", snapshot->error,
                 (unsigned long)((now - snapshot->updated_ms) / 1000));
    }
    const char *warning = network->state != WIFI_LINK_CONNECTED ? "WiFi 未连接，正在等待连接" :
                          cpr_api_auth_denied() ? "管理 Key 鉴权失败，请检查配置" :
                          snapshot->error[0] ? (snapshot->data ? stale : snapshot->error) : battery->available && battery->percent <= 20 ? "设备电量低，请及时充电" : "";
    set_text(alert, warning);
    lv_obj_set_style_text_color(alert, lv_color_hex(RED), 0);
    if (warning[0]) lv_obj_remove_flag(alert, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(alert, LV_OBJ_FLAG_HIDDEN);
    if (on && !board_display_is_touched()) {
        if (summary->version != summary_version) {
            render_overview(summary->data);
            render_health(summary->data);
            summary_version = summary->version;
        }
        if (active_page >= 2) {
            unsigned table = active_page - 2;
            if (table_version[table] != snapshot->version) render_table(res, snapshot);
        }
    }
    cpr_api_unlock();
    if (automatic_pages && on && !board_display_is_touched()) {
        if (active_page == 0 && !overview_refresh_started) {
            cpr_api_refresh_all();
            overview_refresh_started = overview_waiting = true;
            cycle.refreshing = true;
        }
        if (active_page == 0 && overview_waiting) {
            page_started_ms = now;
            if (!cycle.refreshing) overview_waiting = false;
        }
        if (now - page_started_ms >= interval * 1000) cpr_ui_select_page(active_page + 1);
    }
}
