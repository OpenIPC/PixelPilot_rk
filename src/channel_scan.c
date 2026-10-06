#include "channel_scan.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gsmenu/styles.h"

extern lv_obj_t *pp_menu_screen;
extern lv_indev_t *indev_drv;

lv_group_t *channel_scan_group;
lv_group_t *channel_scan_return_group;

static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t scan_thread;
static bool scan_running;
static bool scan_thread_started;
static channel_scan_row_t scan_rows[CHANNEL_SCAN_MAX_ROWS];
static int scan_row_count;

static lv_obj_t *scan_status;
static lv_obj_t *scan_chart;
static lv_timer_t *scan_timer;

typedef struct {
    lv_obj_t *column;
    lv_obj_t *activity;
    lv_obj_t *channel;
} scan_column_t;

static scan_column_t columns[CHANNEL_SCAN_MAX_ROWS];
static int columns_created;

static void sort_rows(channel_scan_row_t *rows, int count)
{
    for(int i = 1; i < count; i++) {
        channel_scan_row_t item = rows[i];
        int j = i - 1;
        while(j >= 0 && rows[j].activity < item.activity) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = item;
    }
}

static void read_results(void)
{
    FILE *pipe = popen("gsmenu.sh get gs wfbng chanscan_results", "r");
    channel_scan_row_t rows[CHANNEL_SCAN_MAX_ROWS];
    int count = 0;

    if(pipe) {
        char line[128];
        while(count < CHANNEL_SCAN_MAX_ROWS && fgets(line, sizeof(line), pipe)) {
            int channel, activity;
            if(sscanf(line, "%d:%d", &channel, &activity) != 2) continue;
            if(channel < 1 || activity < 0) continue;
            rows[count++] = (channel_scan_row_t){ channel, activity };
        }
        pclose(pipe);
    }

    sort_rows(rows, count);
    pthread_mutex_lock(&scan_mutex);
    memcpy(scan_rows, rows, sizeof(rows[0]) * (size_t)count);
    scan_row_count = count;
    pthread_mutex_unlock(&scan_mutex);
}

static void *scan_worker(void *unused)
{
    (void)unused;
    while(scan_running) {
        read_results();
        for(int i = 0; i < 20 && scan_running; i++) lv_delay_ms(100);
    }
    return NULL;
}

void channel_scan_start(void)
{
    if(scan_thread_started) return;
    scan_running = true;
    scan_thread_started = pthread_create(&scan_thread, NULL, scan_worker, NULL) == 0;
    if(!scan_thread_started) scan_running = false;
}

void channel_scan_stop(void)
{
    if(!scan_thread_started) return;
    scan_running = false;
    pthread_join(scan_thread, NULL);
    scan_thread_started = false;
}

int channel_scan_snapshot(channel_scan_row_t *rows, int max_rows)
{
    if(!rows || max_rows <= 0) return 0;
    pthread_mutex_lock(&scan_mutex);
    int count = scan_row_count < max_rows ? scan_row_count : max_rows;
    memcpy(rows, scan_rows, sizeof(rows[0]) * (size_t)count);
    pthread_mutex_unlock(&scan_mutex);
    return count;
}

static lv_color_t activity_color(int pct)
{
    if(pct < 20) return lv_color_hex(0x2ecc71);
    if(pct < 60) return lv_color_hex(0xf1c40f);
    return lv_color_hex(0xe74c3c);
}

static void ensure_columns(int count)
{
    for(; columns_created < count; columns_created++) {
        scan_column_t *column = &columns[columns_created];
        column->column = lv_obj_create(scan_chart);
        lv_obj_remove_flag(column->column, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_grow(column->column, 1);
        lv_obj_set_height(column->column, LV_PCT(100));
        lv_obj_set_flex_flow(column->column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(column->column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(column->column, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(column->column, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(column->column, 2, LV_PART_MAIN);

        column->activity = lv_label_create(column->column);
        lv_obj_set_style_text_font(column->activity, &lv_font_montserrat_20, LV_PART_MAIN);
        column->channel = lv_label_create(column->column);
        lv_obj_set_style_text_font(column->channel, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_add_style(column->channel, &style_openipc_textcolor, LV_PART_MAIN);
    }
}

static void refresh_chart(lv_timer_t *timer)
{
    (void)timer;
    channel_scan_row_t rows[CHANNEL_SCAN_MAX_ROWS];
    int count = channel_scan_snapshot(rows, CHANNEL_SCAN_MAX_ROWS);
    int max_activity = 1;
    for(int i = 0; i < count; i++) if(rows[i].activity > max_activity) max_activity = rows[i].activity;

    if(count == 0) {
        lv_label_set_text(scan_status, "Scanning... no results yet");
        for(int i = 0; i < columns_created; i++) lv_obj_add_flag(columns[i].column, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int quietest = count - 1;
    char status[96];
    snprintf(status, sizeof(status), LV_SYMBOL_OK " Quietest: CH %d   Activity %d", rows[quietest].channel, rows[quietest].activity);
    lv_label_set_text(scan_status, status);
    ensure_columns(count);

    for(int i = 0; i < count; i++) {
        int pct = rows[i].activity * 100 / max_activity;
        char text[24];
        snprintf(text, sizeof(text), "%d%%", pct);
        lv_label_set_text(columns[i].activity, text);
        lv_obj_set_style_text_color(columns[i].activity, activity_color(pct), LV_PART_MAIN);
        snprintf(text, sizeof(text), "%s%d", i == quietest ? LV_SYMBOL_OK : "", rows[i].channel);
        lv_label_set_text(columns[i].channel, text);
        lv_obj_set_style_text_color(columns[i].channel,
                                    i == quietest ? lv_color_hex(0x4c60d8) : lv_color_hex(0x8a93a6),
                                    LV_PART_MAIN);
        lv_obj_remove_flag(columns[i].column, LV_OBJ_FLAG_HIDDEN);
    }
    for(int i = count; i < columns_created; i++) lv_obj_add_flag(columns[i].column, LV_OBJ_FLAG_HIDDEN);
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    lv_screen_load(pp_menu_screen);
    lv_indev_set_group(indev_drv, channel_scan_return_group);
}

static void loaded_cb(lv_event_t *event)
{
    (void)event;
    channel_scan_start();
    lv_timer_resume(scan_timer);
    refresh_chart(NULL);
}

static void unloaded_cb(lv_event_t *event)
{
    (void)event;
    lv_timer_pause(scan_timer);
    channel_scan_stop();
}

void channel_scan_screen_init(lv_obj_t *parent)
{
    channel_scan_group = lv_group_create();
    lv_group_set_default(channel_scan_group);

    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_size(content, LV_PCT(98), LV_PCT(96));
    lv_obj_center(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(content, &style_openipc_dark_background, LV_PART_MAIN);

    lv_obj_t *controls = lv_obj_create(content);
    lv_obj_set_width(controls, LV_SIZE_CONTENT);
    lv_obj_set_height(controls, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_width(controls, 0, LV_PART_MAIN);
    lv_obj_add_style(controls, &style_openipc_dark_background, LV_PART_MAIN);

    lv_obj_t *back = lv_button_create(controls);
    lv_obj_add_style(back, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(back, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_t *back_text = lv_label_create(back);
    lv_label_set_text(back_text, "Back");
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(channel_scan_group, back);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "WFB-NG Channel Scan (5GHz)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_add_style(title, &style_openipc_textcolor, LV_PART_MAIN);

    scan_status = lv_label_create(content);
    lv_label_set_text(scan_status, "Scanning...");
    lv_obj_add_style(scan_status, &style_openipc_textcolor, LV_PART_MAIN);

    scan_chart = lv_obj_create(content);
    lv_obj_set_width(scan_chart, LV_PCT(100));
    lv_obj_set_flex_grow(scan_chart, 1);
    lv_obj_set_flex_flow(scan_chart, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(scan_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scan_chart, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scan_chart, 4, LV_PART_MAIN);
    lv_obj_add_style(scan_chart, &style_openipc_dark_background, LV_PART_MAIN);

    scan_timer = lv_timer_create(refresh_chart, 500, NULL);
    lv_timer_pause(scan_timer);
    lv_obj_add_event_cb(parent, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(parent, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
