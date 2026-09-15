/*
 * Spectrum Scanner screen — displays live rtl88x2eu channel-utilization stats
 * (see ../spectrum_scanner.hpp) as a single-screen, no-scroll bar graph (one
 * vertical bar per 5GHz channel), since the GS's 4-way joystick can't scroll
 * a list. In the same "dedicated full screen" style as the TX-Profiles /
 * DVR-Player screens (see air_txprofiles.c / gs_dvrplayer.c).
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "spectrum_screen.h"
#include "styles.h"
#include "helper.h"
#include "../spectrum_scanner.hpp"

extern lv_obj_t *   pp_menu_screen;
extern lv_indev_t * indev_drv;

lv_group_t * spectrum_group;          /* screen's own input group            */
lv_group_t * spectrum_return_group;   /* set by the menu before launching    */

/* One pre-built column per possible channel; created lazily as the scan
 * reports more channels, then just shown/hidden/updated on every refresh so
 * the graph doesn't tear down and rebuild its widgets every 500ms. */
typedef struct {
    lv_obj_t * col;
    lv_obj_t * quality_dot;
    lv_obj_t * pct_label;
    lv_obj_t * bar;
    lv_obj_t * ch_label;
    lv_obj_t * freq_label;
} col_widgets_t;

static lv_obj_t *    chart_row;    /* flex row of channel columns, one screen, no scroll */
static lv_obj_t *    iface_label;
static lv_obj_t *    recommend_label;
static lv_obj_t *    empty_label;  /* "no data" placeholder inside chart_row */
static lv_timer_t *  refresh_timer;
static col_widgets_t cols[SPECTRUM_MAX_ROWS];
static int           cols_created;

static spectrum_iface_t g_snapshot[SPECTRUM_MAX_IFACES];
static int g_iface_count;
static int g_iface_idx;

/* Utilization thresholds mirror the source script's activity-bar coloring;
 * quality/availability mirror its "green >=90, yellow >=70, else red" rule. */
static lv_color_t util_color(int pct) {
    if (pct < 20) return lv_color_hex(0x2ecc71);
    if (pct < 60) return lv_color_hex(0xf1c40f);
    return lv_color_hex(0xe74c3c);
}
static lv_color_t quality_color(int pct) {
    if (pct >= 90) return lv_color_hex(0x2ecc71);
    if (pct >= 70) return lv_color_hex(0xf1c40f);
    return lv_color_hex(0xe74c3c);
}

static void ensure_col_widgets(int count) {
    for (; cols_created < count; cols_created++) {
        col_widgets_t * cw = &cols[cols_created];

        cw->col = lv_obj_create(chart_row);
        lv_obj_remove_flag(cw->col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_grow(cw->col, 1);
        lv_obj_set_height(cw->col, LV_PCT(100));
        lv_obj_set_flex_flow(cw->col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cw->col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(cw->col, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(cw->col, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cw->col, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_row(cw->col, 2, LV_PART_MAIN);

        cw->quality_dot = lv_obj_create(cw->col);
        lv_obj_remove_flag(cw->quality_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cw->quality_dot, 10, 10);
        lv_obj_set_style_radius(cw->quality_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(cw->quality_dot, 0, LV_PART_MAIN);

        cw->pct_label = lv_label_create(cw->col);
        lv_obj_set_style_text_font(cw->pct_label, &lv_font_montserrat_20, LV_PART_MAIN);

        cw->bar = lv_bar_create(cw->col);
        lv_obj_set_width(cw->bar, LV_PCT(55));
        lv_obj_set_flex_grow(cw->bar, 1);
        lv_bar_set_range(cw->bar, 0, 100);
        lv_obj_set_style_bg_color(cw->bar, lv_color_hex(0x080a10), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cw->bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(cw->bar, 3, LV_PART_MAIN | LV_PART_INDICATOR);

        cw->ch_label = lv_label_create(cw->col);
        lv_obj_set_style_text_font(cw->ch_label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_add_style(cw->ch_label, &style_openipc_textcolor, LV_PART_MAIN);

        cw->freq_label = lv_label_create(cw->col);
        lv_obj_add_style(cw->freq_label, &style_openipc_textcolor, LV_PART_MAIN);
    }
}

static void update_col(int idx, const spectrum_channel_t * ch, bool best) {
    col_widgets_t * cw = &cols[idx];
    char buf[16];

    lv_obj_set_style_bg_color(cw->quality_dot, quality_color(ch->quality_pct), LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "%d%%", ch->util_pct);
    lv_label_set_text(cw->pct_label, buf);
    lv_obj_set_style_text_color(cw->pct_label, util_color(ch->util_pct), LV_PART_MAIN);

    /* Bar height tracks utilization directly — a tall bar means a busy,
     * best-avoided channel. */
    lv_bar_set_value(cw->bar, ch->util_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(cw->bar, util_color(ch->util_pct), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%s%d", best ? LV_SYMBOL_OK : "", ch->channel);
    lv_label_set_text(cw->ch_label, buf);
    lv_obj_set_style_text_color(cw->ch_label, best ? lv_color_hex(0x4c60d8) : lv_color_hex(0x8a93a6), LV_PART_MAIN);

    if (ch->freq_mhz > 0) snprintf(buf, sizeof(buf), "%d", ch->freq_mhz);
    else                  buf[0] = '\0';
    lv_label_set_text(cw->freq_label, buf);

    lv_obj_remove_flag(cw->col, LV_OBJ_FLAG_HIDDEN);
}

/* 5GHz only, per the GS operator's request — 2.4GHz is rarely usable for FPV
 * video anyway and left off to keep every channel visible without scrolling. */
static bool is_5ghz(const spectrum_channel_t * ch) {
    if (ch->freq_mhz > 0) return ch->freq_mhz >= 5000;
    return ch->channel >= 32;
}

/* Best channel = lowest utilization (least busy air time), ties broken by
 * highest quality. Mirrors the source script's own quality/utilization
 * columns rather than inventing a new scoring metric. */
static int pick_best(const spectrum_channel_t * rows, int count) {
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (best < 0 || rows[i].util_pct < rows[best].util_pct ||
            (rows[i].util_pct == rows[best].util_pct && rows[i].quality_pct > rows[best].quality_pct)) {
            best = i;
        }
    }
    return best;
}

static void refresh_chart(void) {
    g_iface_count = spectrum_scanner_snapshot(g_snapshot, SPECTRUM_MAX_IFACES);
    if (g_iface_idx >= g_iface_count) g_iface_idx = 0;

    if (g_iface_count == 0) {
        lv_label_set_text(iface_label, "No rtl88x2eu-family adapter found");
        lv_label_set_text(recommend_label, "");
        for (int i = 0; i < cols_created; i++) lv_obj_add_flag(cols[i].col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(empty_label, "Plug in a supported adapter and reopen this screen.");
        return;
    }

    const spectrum_iface_t * ifc = &g_snapshot[g_iface_idx];
    char title[80];
    snprintf(title, sizeof(title), "%s   (%d/%d)   BW %s", ifc->name, g_iface_idx + 1, g_iface_count,
             ifc->bandwidth[0] ? ifc->bandwidth : "?");
    lv_label_set_text(iface_label, title);

    spectrum_channel_t band[SPECTRUM_MAX_ROWS];
    int n = 0;
    for (int r = 0; r < ifc->row_count && n < SPECTRUM_MAX_ROWS; r++)
        if (is_5ghz(&ifc->rows[r])) band[n++] = ifc->rows[r];

    if (n == 0) {
        lv_label_set_text(recommend_label, "");
        for (int i = 0; i < cols_created; i++) lv_obj_add_flag(cols[i].col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(empty_label, "Scanning...  no 5GHz channel data yet.");
        return;
    }

    lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
    int best = pick_best(band, n);
    int busy_sum = 0;
    for (int i = 0; i < n; i++) busy_sum += band[i].util_pct;
    char rec[160];
    snprintf(rec, sizeof(rec),
             LV_SYMBOL_OK " Best: CH %d (%d MHz) — %d%% busy, %d%% quality, %d%% avail, %d%% interf   |   band avg %d%% busy",
             band[best].channel, band[best].freq_mhz, band[best].util_pct, band[best].quality_pct,
             band[best].availability_pct, band[best].interference_pct, busy_sum / n);
    lv_label_set_text(recommend_label, rec);

    ensure_col_widgets(n);
    for (int i = 0; i < n; i++) update_col(i, &band[i], i == best);
    for (int i = n; i < cols_created; i++) lv_obj_add_flag(cols[i].col, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_timer_cb(lv_timer_t * t) {
    (void)t;
    refresh_chart();
}

static void next_iface_cb(lv_event_t * e) {
    (void)e;
    if (g_iface_count > 0) g_iface_idx = (g_iface_idx + 1) % g_iface_count;
    refresh_chart();
}

static void back_cb(lv_event_t * e) {
    (void)e;
    lv_screen_load(pp_menu_screen);
    lv_indev_set_group(indev_drv, spectrum_return_group);
}

static void screen_loaded_cb(lv_event_t * e) {
    (void)e;
    spectrum_scanner_start();
    if (refresh_timer) lv_timer_resume(refresh_timer);
    refresh_chart();
}

static void screen_unloaded_cb(lv_event_t * e) {
    (void)e;
    if (refresh_timer) lv_timer_pause(refresh_timer);
    spectrum_scanner_stop();
}

void spectrum_screen_init(lv_obj_t * parent) {
    spectrum_group = lv_group_create();
    lv_group_set_default(spectrum_group);

    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(98), LV_PCT(96));
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cont, 6, LV_PART_MAIN);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(cont, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(cont, &style_openipc_dark_background, LV_PART_MAIN);

    lv_obj_t * button_cont = lv_obj_create(cont);
    lv_obj_set_width(button_cont, LV_SIZE_CONTENT);
    lv_obj_set_height(button_cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(button_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(button_cont, 0);
    lv_obj_add_style(button_cont, &style_openipc_dark_background, LV_PART_MAIN);
    lv_obj_set_style_border_width(button_cont, 0, LV_PART_MAIN);

    lv_obj_t * back_btn = lv_button_create(button_cont);
    lv_obj_add_style(back_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(back_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * next_btn = lv_button_create(button_cont);
    lv_obj_add_style(next_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(next_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_t * next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, "Next Adapter");
    lv_obj_add_event_cb(next_btn, next_iface_cb, LV_EVENT_CLICKED, NULL);

    iface_label = lv_label_create(cont);
    lv_label_set_text(iface_label, "Scanning...");
    lv_obj_set_width(iface_label, LV_PCT(100));
    lv_label_set_long_mode(iface_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(iface_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_add_style(iface_label, &style_openipc_textcolor, LV_PART_MAIN);

    recommend_label = lv_label_create(cont);
    lv_label_set_text(recommend_label, "");
    lv_obj_set_width(recommend_label, LV_PCT(100));
    lv_label_set_long_mode(recommend_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(recommend_label, &lv_font_montserrat_20, LV_PART_MAIN);

    chart_row = lv_obj_create(cont);
    lv_obj_set_width(chart_row, LV_PCT(100));
    lv_obj_set_flex_grow(chart_row, 1);
    lv_obj_set_flex_flow(chart_row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(chart_row, LV_OBJ_FLAG_SCROLLABLE);   /* everything must fit — no joystick scrolling */
    lv_obj_set_style_pad_all(chart_row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(chart_row, 4, LV_PART_MAIN);
    lv_obj_add_style(chart_row, &style_openipc_dark_background, LV_PART_MAIN);
    lv_obj_add_style(chart_row, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    empty_label = lv_label_create(chart_row);
    lv_label_set_text(empty_label, "Scanning...");
    lv_obj_add_style(empty_label, &style_openipc_textcolor, LV_PART_MAIN);

    lv_group_add_obj(spectrum_group, back_btn);
    lv_group_add_obj(spectrum_group, next_btn);
    lv_group_add_obj(spectrum_group, chart_row);

    refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
    lv_timer_pause(refresh_timer); /* resumed on LV_EVENT_SCREEN_LOADED */

    lv_obj_add_event_cb(parent, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(parent, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
