#include "column_stack.h"
#include "styles.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Provided by the app (input.cpp / menu.c). The stack retargets this indev's
 * group to the right-most column so keys always hit the visible level. */
extern lv_indev_t * indev_drv;

/* The project's control mode. We flip it to SLIDER while editing a slider so
 * the WASD mapping in input.cpp routes +/- to us; NAV otherwise. */
#include "../input.h"
extern gsmenu_control_mode_t control_mode;

#define CS_MAX_COLS   8
#define CS_COL_WIDTH  360

typedef enum {
    CS_SUBMENU,
    CS_SWITCH,
    CS_SLIDER,
    CS_DROPDOWN,
    CS_RADIO,
    CS_ACTION,
    CS_TEXT,
} cs_role_t;

typedef struct {
    cs_role_t          role;
    colstack_t *       cs;
    const char *       param;       /* bound gsmenu.sh param (for external reflect) */
    lv_obj_t *         label;       /* main text label (measured for autofit)    */
    /* submenu */
    colstack_builder_t builder;
    void *             user;
    char               title[64];
    /* switch */
    lv_obj_t *         sw;
    /* slider */
    lv_obj_t *         slider;
    lv_obj_t *         slider_val;
    int                precision;   /* slider decimals (value = raw/10^prec)     */
    int                display_scale; /* multiply displayed value by this (e.g., 100 for pos*100 MB) */
    /* dropdown */
    lv_obj_t *         value_lbl;   /* the "current value" text on the row      */
    char *             options;     /* '\n' separated, owned                    */
    int                sel;
    /* radio */
    lv_obj_t *         owner;       /* dropdown row this radio option belongs to */
    int                index;
    /* value binding (optional): fired with the new value string on change */
    void            (* on_change)(void * ctx, const char * value);
    void *             ctx;
    /* live slider preview (optional): fired with the value on every adjustment
     * while editing, and again with the original value on cancel */
    void            (* on_live)(void * ctx, const char * value);
    void *             live_ctx;
    /* action binding (optional): fired on activate */
    void            (* on_activate)(void * ctx);
    void *             act_ctx;
} cs_item_t;

struct colstack {
    lv_obj_t * stack;
    struct {
        lv_obj_t *   col;
        lv_obj_t *   body;
        lv_group_t * group;
        lv_obj_t *   origin;   /* the row that opened this column               */
    } cols[CS_MAX_COLS];
    int         depth;        /* number of live columns; root included         */
    lv_obj_t *  edit_slider;  /* non-NULL while a slider is being edited        */
    cs_item_t * edit_item;    /* the item owning edit_slider                    */
    int32_t     edit_start;   /* slider value on entering edit (for cancel)     */
    void     (* root_back)(void * ctx);  /* back pressed at the root column       */
    void *      root_back_ctx;
};

/* ─────────────────────────────────────────────────────────────────────────── */

static void refresh_active_styling(colstack_t * cs)
{
    for(int i = 0; i < cs->depth; i++) {
        bool active = (i == cs->depth - 1);
        /* constant border WIDTH (only the colour changes) so a column's content
         * doesn't shift by a pixel when it goes from active to inactive. */
        lv_obj_set_style_border_width(cs->cols[i].col, 2, 0);
        lv_obj_set_style_border_color(cs->cols[i].col,
                                      active ? lv_color_hex(0x4c60d8) : lv_color_hex(0x39424f), 0);
        /* de-emphasise columns the user has stepped back from — but only slightly,
         * so their text stays readable (LV_OPA_70 washed them out over the video). */
        lv_obj_set_style_opa(cs->cols[i].col, active ? LV_OPA_COVER : LV_OPA_90, 0);
    }
}

static lv_obj_t * make_column(colstack_t * cs, const char * title)
{
    lv_obj_t * col = lv_obj_create(cs->stack);
    /* Height fits the content (short pages don't waste the whole screen) but is
     * capped at the viewport so long lists still bound their scroll area. */
    lv_obj_set_width(col, CS_COL_WIDTH);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(col, LV_PCT(100), 0);
    lv_obj_set_style_bg_color(col, lv_color_hex(0x0b0e14), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    /* light text inherited by all rows — otherwise the default (dark) theme text
     * on the dark column reads as if every item is disabled */
    lv_obj_set_style_text_color(col, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_radius(col, 8, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLL_ELASTIC);

    /* column header */
    lv_obj_t * hdr = lv_label_create(col);
    lv_label_set_text(hdr, title ? title : "");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_pad_all(hdr, 10, 0);
    lv_obj_set_width(hdr, LV_PCT(100));

    /* scrollable body that holds the rows. Height fits the rows (so the column
     * wraps its content) but is capped in pixels at the viewport minus the
     * header, so a long list bounds its own scroll area while the header stays
     * put. NOTE: flex_grow does NOT work under a SIZE_CONTENT parent (the child
     * collapses), so the body must be SIZE_CONTENT with an explicit max. */
    lv_obj_t * body = lv_obj_create(col);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_update_layout(cs->stack);
    int32_t vh = lv_obj_get_content_height(cs->stack);
    if(vh > 100) lv_obj_set_style_max_height(body, vh - 52 /*header+pads*/, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_user_data(col, body);
    return col;
}

lv_obj_t * colstack_push(colstack_t * cs, const char * title)
{
    if(cs->depth >= CS_MAX_COLS) return NULL;

    lv_obj_t * col   = make_column(cs, title);
    lv_obj_t * body  = lv_obj_get_user_data(col);
    lv_group_t * grp = lv_group_create();

    int i = cs->depth++;
    cs->cols[i].col   = col;
    cs->cols[i].body  = body;
    cs->cols[i].group = grp;

    lv_indev_set_group(indev_drv, grp);
    lv_obj_scroll_to_view(col, LV_ANIM_ON);
    refresh_active_styling(cs);
    return body;
}

void colstack_pop(colstack_t * cs)
{
    if(cs->depth <= 1) return;            /* never pop the root */

    int i = --cs->depth;
    lv_group_delete(cs->cols[i].group);
    lv_obj_delete(cs->cols[i].col);
    cs->cols[i].col = NULL;

    lv_indev_set_group(indev_drv, cs->cols[cs->depth - 1].group);
    lv_obj_scroll_to_view(cs->cols[cs->depth - 1].col, LV_ANIM_ON);
    refresh_active_styling(cs);
}

int colstack_depth(colstack_t * cs) { return cs->depth; }

lv_obj_t * colstack_root(colstack_t * cs) { return cs->cols[0].body; }

lv_group_t * colstack_cur_group(colstack_t * cs) { return cs->cols[cs->depth - 1].group; }

/* ── key handling ──────────────────────────────────────────────────────────── */

static bool is_back_key(uint32_t k)    { return k == LV_KEY_LEFT || k == LV_KEY_ESC || k == LV_KEY_HOME; }
static bool is_forward_key(uint32_t k) { return k == LV_KEY_RIGHT || k == LV_KEY_ENTER; }

static void fire_change(cs_item_t * it, const char * value)
{
    if(it && it->on_change) it->on_change(it->ctx, value);
}

/* Live slider preview: format the raw value and fire the on_live callback (for
 * effects that should track the slider as it moves, e.g. DRM video scaling). */
static void fire_slider_live(cs_item_t * it, int32_t raw)
{
    if(!it || !it->on_live) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", it->precision, raw / pow(10, it->precision));
    it->on_live(it->live_ctx, buf);
}

/* Announce slider edit mode on the slider itself: the bar grows taller and gains
 * a bright white border + knob, clearly stronger than the row's focus outline
 * (an outline gets clipped by the row, so we use size + border instead). */
static void slider_edit_style(lv_obj_t * s, bool on)
{
    lv_obj_set_height(s, on ? 20 : 10);
    lv_obj_set_style_border_width(s, on ? 3 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(s, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, on ? lv_color_hex(0xffffff) : lv_color_hex(0x4c60d8), LV_PART_KNOB);
}

static void leave_slider_edit(colstack_t * cs)
{
    if(cs->edit_slider) slider_edit_style(cs->edit_slider, false);
    cs->edit_slider = NULL;
    cs->edit_item = NULL;
    control_mode = GSMENU_CONTROL_MODE_NAV;
}

static void commit_slider_edit(colstack_t * cs)
{
    if(cs->edit_item && cs->edit_slider) {
        double v = lv_slider_get_value(cs->edit_slider) / pow(10, cs->edit_item->precision);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", cs->edit_item->precision, v);
        fire_change(cs->edit_item, buf);
    }
    leave_slider_edit(cs);
}

/* cancel: restore the value we started with, without firing the binding */
static void cancel_slider_edit(colstack_t * cs)
{
    if(cs->edit_slider) {
        lv_slider_set_value(cs->edit_slider, cs->edit_start, LV_ANIM_OFF);
        if(cs->edit_item && cs->edit_item->slider_val) {
            double f = pow(10, cs->edit_item->precision);
            double displayed = (cs->edit_start / f) * (cs->edit_item->display_scale ? cs->edit_item->display_scale : 1);
            lv_label_set_text_fmt(cs->edit_item->slider_val, "%.*f",
                                  cs->edit_item->precision, displayed);
        }
        fire_slider_live(cs->edit_item, cs->edit_start);   /* revert live preview */
    }
    leave_slider_edit(cs);
}

/* deferred radio apply — deleting the focused row inside its own key event is a
 * use-after-free, so pop + fire the binding on the next LVGL cycle. */
typedef struct { colstack_t * cs; cs_item_t * owner; int index; char text[64]; } cs_radio_apply_t;

static void cs_radio_apply_cb(void * p)
{
    cs_radio_apply_t * a = p;
    if(a->owner && a->owner->value_lbl) {
        lv_label_set_text(a->owner->value_lbl, a->text);
        a->owner->sel = a->index;
    }
    colstack_pop(a->cs);
    colstack_autofit(a->cs);   /* re-fit the column now the value changed length */
    fire_change(a->owner, a->text);
    free(a);
}

static void item_key_cb(lv_event_t * e)
{
    cs_item_t * it  = lv_event_get_user_data(e);
    colstack_t * cs = it->cs;
    uint32_t k      = lv_event_get_key(e);

    /* Editing a slider: RIGHT/LEFT adjust, ENTER commits, Back (ESC) reverts. */
    if(cs->edit_slider) {
        if(k == LV_KEY_RIGHT || k == LV_KEY_LEFT) {
            int32_t v   = lv_slider_get_value(cs->edit_slider);
            int32_t min = lv_slider_get_min_value(cs->edit_slider);
            int32_t max = lv_slider_get_max_value(cs->edit_slider);
            int32_t step = (max - min) / 20; if(step < 1) step = 1;
            v += (k == LV_KEY_RIGHT) ? step : -step;
            if(v < min) v = min; if(v > max) v = max;
            lv_slider_set_value(cs->edit_slider, v, LV_ANIM_OFF);
            if(it->slider_val) {
                double f = pow(10, it->precision);
                double displayed = (v / f) * (it->display_scale ? it->display_scale : 1);
                lv_label_set_text_fmt(it->slider_val, "%.*f", it->precision, displayed);
            }
            fire_slider_live(it, v);   /* live-preview the effect as it moves */
        }
        else if(k == LV_KEY_ENTER) {
            commit_slider_edit(cs);
        }
        else {                       /* ESC / HOME → cancel, revert to original */
            cancel_slider_edit(cs);
        }
        return;
    }

    /* disabled rows (e.g. drone pages when the VTX isn't detected) can't be
     * entered/activated; Back still works to leave. */
    if(is_forward_key(k) && lv_obj_has_state(lv_event_get_target(e), LV_STATE_DISABLED))
        return;

    if(is_forward_key(k)) {
        switch(it->role) {
            case CS_SUBMENU: {
                lv_obj_t * body = colstack_push(cs, it->title);
                if(body) cs->cols[cs->depth - 1].origin = lv_event_get_target(e);
                if(body && it->builder) it->builder(cs, body, it->user);
                break;
            }
            case CS_SWITCH: {
                bool on = !lv_obj_has_state(it->sw, LV_STATE_CHECKED);
                if(on) lv_obj_add_state(it->sw, LV_STATE_CHECKED);
                else   lv_obj_remove_state(it->sw, LV_STATE_CHECKED);
                /* gsmenu.sh set expects on/off for switches (get returns 0/1). */
                fire_change(it, on ? "on" : "off");
                break;
            }
            case CS_SLIDER: {
                cs->edit_slider = it->slider;
                cs->edit_item   = it;
                cs->edit_start  = lv_slider_get_value(it->slider);
                slider_edit_style(it->slider, true);
                control_mode = GSMENU_CONTROL_MODE_SLIDER;
                break;
            }
            case CS_DROPDOWN: {
                /* open the options as a radio list in a new column */
                lv_obj_t * body = colstack_push(cs, it->title);
                if(!body) break;
                char * opts = strdup(it->options ? it->options : "");
                int idx = 0;
                char * save = NULL;
                lv_obj_t * sel_row = NULL;
                for(char * tok = strtok_r(opts, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
                    lv_obj_t * r = colstack_add_action(cs, body, NULL, tok);
                    cs_item_t * ri = lv_obj_get_user_data(r);
                    ri->role  = CS_RADIO;
                    ri->index = idx;
                    /* radio bullet on the current value */
                    lv_obj_t * bullet = lv_label_create(r);
                    lv_label_set_text(bullet, idx == it->sel ? LV_SYMBOL_OK : "");
                    lv_obj_set_style_text_color(bullet, lv_color_hex(0x4c60d8), 0);
                    /* remember which dropdown this belongs to */
                    ri->owner = lv_event_get_target(e);
                    if(idx == it->sel) sel_row = r;
                    idx++;
                }
                free(opts);
                colstack_autofit(cs);
                if(sel_row) {
                    lv_group_focus_obj(sel_row);            /* open on current value */
                    /* centre it so options above and below are visible (clamped
                     * to the ends when near the top/bottom) */
                    lv_obj_update_layout(body);
                    lv_area_t rc, bc;
                    lv_obj_get_coords(sel_row, &rc);
                    lv_obj_get_coords(body, &bc);
                    int32_t diff = (rc.y1 + rc.y2) / 2 - (bc.y1 + bc.y2) / 2;
                    lv_obj_scroll_to_y(body, lv_obj_get_scroll_y(body) + diff, LV_ANIM_OFF);
                }
                break;
            }
            case CS_RADIO: {
                cs_item_t * owner = it->owner ? lv_obj_get_user_data(it->owner) : NULL;
                lv_obj_t * txt = lv_obj_get_child(lv_event_get_target(e), 0);
                cs_radio_apply_t * a = calloc(1, sizeof(cs_radio_apply_t));
                a->cs = cs; a->owner = owner; a->index = it->index;
                if(txt) strncpy(a->text, lv_label_get_text(txt), sizeof(a->text) - 1);
                lv_async_call(cs_radio_apply_cb, a);   /* deferred: safe delete + bind */
                break;
            }
            case CS_ACTION:
                /* Fire on the KEY press, NOT via LV_EVENT_CLICKED: the keypad
                 * sends CLICKED on ENTER *release*, which after a column push
                 * would land on the child column's freshly focused row. */
                if(it->on_activate) it->on_activate(it->act_ctx);
                break;
        }
        return;
    }

    if(is_back_key(k)) {
        if(cs->depth <= 1) { if(cs->root_back) cs->root_back(cs->root_back_ctx); }
        else               colstack_pop(cs);
    }
}

static void item_delete_cb(lv_event_t * e)
{
    cs_item_t * it = lv_event_get_user_data(e);
    if(it) { free(it->options); free(it); }
}

/* ── row construction ──────────────────────────────────────────────────────── */

static lv_obj_t * base_row(colstack_t * cs, lv_obj_t * col, const char * icon,
                           const char * text, cs_item_t ** out_it)
{
    lv_obj_t * row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161b24), 0);
    lv_obj_set_style_bg_opa(row, 200, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_margin_bottom(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(row, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(row, &style_openipc_disabled, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if(icon) {
        lv_obj_t * ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_color(ic, lv_color_hex(0xf2f4f8), 0);
        lv_obj_set_style_pad_right(ic, 8, 0);
    }

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, text ? text : "");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);   /* wrap only if it must */
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_flex_grow(lbl, 1);

    cs_item_t * it = calloc(1, sizeof(cs_item_t));
    it->cs = cs;
    it->label = lbl;
    if(text) { strncpy(it->title, text, sizeof(it->title) - 1); }
    lv_obj_set_user_data(row, it);

    /* Keypad-focusable, but NOT clickable: we drive everything from LV_EVENT_KEY.
     * Leaving rows clickable makes LVGL fire a native CLICKED on ENTER, which —
     * after a forward key pushes a new column — lands on the freshly focused row
     * of that column (spurious activation). */
    lv_group_t * grp = cs->cols[cs->depth - 1].group;
    lv_group_add_obj(grp, row);
    lv_obj_add_event_cb(row, item_key_cb, LV_EVENT_KEY, it);
    lv_obj_add_event_cb(row, item_delete_cb, LV_EVENT_DELETE, it);

    if(out_it) *out_it = it;
    return row;
}

lv_obj_t * colstack_add_title(colstack_t * cs, lv_obj_t * col, const char * text)
{
    (void)cs;
    lv_obj_t * lbl = lv_label_create(col);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8a93a6), 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 2, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    return lbl;
}

lv_obj_t * colstack_add_submenu(colstack_t * cs, lv_obj_t * col, const char * icon,
                                const char * text, colstack_builder_t builder, void * user)
{
    cs_item_t * it;
    lv_obj_t * row = base_row(cs, col, icon, text, &it);
    it->role = CS_SUBMENU;
    it->builder = builder;
    it->user = user;
    lv_obj_t * chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(0x8a93a6), 0);
    return row;
}

lv_obj_t * colstack_add_switch(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text, bool on)
{
    cs_item_t * it;
    lv_obj_t * row = base_row(cs, col, icon, text, &it);
    it->role = CS_SWITCH;
    lv_obj_t * sw = lv_switch_create(row);
    /* lv_switch auto-adds itself to the default group (group_def=TRUE); the first
     * one then shows a stray focus outline. The row is what's navigable, not the
     * switch, so take it out of the group. */
    lv_group_remove_obj(sw);
    lv_obj_add_style(sw, &style_openipc, LV_PART_INDICATOR | LV_STATE_CHECKED);
    /* consistent "cornery" look in both states (not just when checked) */
    lv_obj_set_style_radius(sw, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 6, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    it->sw = sw;
    return row;
}

lv_obj_t * colstack_add_slider(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text, int32_t min, int32_t max, int32_t val,
                               int precision, int display_scale)
{
    cs_item_t * it;
    lv_obj_t * row = base_row(cs, col, icon, text, &it);
    it->role = CS_SLIDER;
    it->precision = precision;
    it->display_scale = display_scale;

    lv_obj_t * val_lbl = lv_label_create(row);
    double f = pow(10, precision);
    double displayed = (val / f) * (display_scale ? display_scale : 1);
    lv_label_set_text_fmt(val_lbl, "%.*f", precision, displayed);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xf2f4f8), 0);
    lv_obj_set_style_pad_right(val_lbl, 8, 0);

    lv_obj_t * slider = lv_slider_create(row);
    lv_obj_set_width(slider, 120);
    lv_obj_set_height(slider, 10);   /* baseline; grows in edit mode */
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);
    lv_obj_add_style(slider, &style_openipc, LV_PART_INDICATOR);
    lv_obj_add_style(slider, &style_openipc, LV_PART_KNOB);
    /* lv_slider also auto-joins the default group (group_def=TRUE); the row is
     * what's navigable, so keep the slider out of the group (avoids a stray
     * focus outline on the first slider of a page). */
    lv_group_remove_obj(slider);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    it->slider = slider;
    it->slider_val = val_lbl;
    return row;
}

lv_obj_t * colstack_add_dropdown(colstack_t * cs, lv_obj_t * col, const char * icon,
                                 const char * text, const char * options, int sel)
{
    cs_item_t * it;
    lv_obj_t * row = base_row(cs, col, icon, text, &it);
    it->role = CS_DROPDOWN;
    it->options = strdup(options ? options : "");
    it->sel = sel;

    /* current value shown on the right, then a chevron (opens a column). Cap its
     * width so a very long value wraps within the cap instead of squeezing the
     * label to nothing. */
    lv_obj_t * val = lv_label_create(row);
    lv_obj_set_style_text_color(val, lv_color_hex(0x4c60d8), 0);
    lv_obj_set_style_pad_right(val, 8, 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(val, 380, 0);
    /* pick the sel-th option for the initial text */
    {
        char * tmp = strdup(options ? options : "");
        char * save = NULL; int i = 0; const char * chosen = "";
        for(char * t = strtok_r(tmp, "\n", &save); t; t = strtok_r(NULL, "\n", &save), i++)
            if(i == sel) { chosen = t; break; }
        lv_label_set_text(val, chosen);
        /* chosen points into tmp; copy before free */
        char buf[64]; strncpy(buf, lv_label_get_text(val), sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
        lv_label_set_text(val, buf);
        free(tmp);
    }
    it->value_lbl = val;

    lv_obj_t * chev = lv_label_create(row);
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chev, lv_color_hex(0x8a93a6), 0);
    return row;
}

lv_obj_t * colstack_add_action(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text)
{
    cs_item_t * it;
    lv_obj_t * row = base_row(cs, col, icon, text, &it);
    it->role = CS_ACTION;
    return row;
}

void colstack_set_binding(lv_obj_t * row, void (*on_change)(void * ctx, const char * value),
                          void * ctx)
{
    cs_item_t * it = lv_obj_get_user_data(row);
    if(it) { it->on_change = on_change; it->ctx = ctx; }
}

void colstack_set_row_param(lv_obj_t * row, const char * param)
{
    cs_item_t * it = lv_obj_get_user_data(row);
    if(it) it->param = param;
}

void colstack_reflect_switch(colstack_t * cs, const char * param, bool on)
{
    if(!param) return;
    /* search every visible column so it still works if the user has stepped into
     * a child column (e.g. an open dropdown) of the page holding the switch. */
    for(int c = 0; c < cs->depth; c++) {
        lv_obj_t * body = cs->cols[c].body;
        uint32_t n = lv_obj_get_child_count(body);
        for(uint32_t i = 0; i < n; i++) {
            lv_obj_t * row = lv_obj_get_child(body, i);
            cs_item_t * it = lv_obj_get_user_data(row);
            if(!it || it->role != CS_SWITCH || !it->sw || !it->param) continue;
            if(strcmp(it->param, param) != 0) continue;
            if(on) lv_obj_add_state(it->sw, LV_STATE_CHECKED);
            else   lv_obj_remove_state(it->sw, LV_STATE_CHECKED);
            return;
        }
    }
}

void colstack_set_slider_live(lv_obj_t * row, void (*on_live)(void * ctx, const char * value),
                              void * ctx)
{
    cs_item_t * it = lv_obj_get_user_data(row);
    if(it) { it->on_live = on_live; it->live_ctx = ctx; }
}

void colstack_set_action(lv_obj_t * row, void (*on_activate)(void * ctx), void * ctx)
{
    cs_item_t * it = lv_obj_get_user_data(row);
    if(it) { it->on_activate = on_activate; it->act_ctx = ctx; }
}

void colstack_set_root_back(colstack_t * cs, void (*cb)(void * ctx), void * ctx)
{
    cs->root_back = cb;
    cs->root_back_ctx = ctx;
}

lv_group_t * colstack_root_group(colstack_t * cs) { return cs->cols[0].group; }

lv_obj_t * colstack_column_origin(colstack_t * cs, int idx)
{
    if(idx < 0 || idx >= cs->depth) return NULL;
    return cs->cols[idx].origin;
}

/* Re-run the builder of the top (non-root) column into a freshly-cleared body —
 * used to refresh a dynamic list in place (e.g. a wifi re-scan). */
void colstack_rebuild_current(colstack_t * cs)
{
    if(cs->depth <= 1) return;
    int i = cs->depth - 1;
    lv_obj_t * origin = cs->cols[i].origin;
    cs_item_t * it = origin ? lv_obj_get_user_data(origin) : NULL;
    if(!it || !it->builder) return;
    lv_obj_clean(cs->cols[i].body);
    it->builder(cs, cs->cols[i].body, it->user);
}

void colstack_set_enabled(lv_obj_t * row, bool enabled)
{
    if(enabled) lv_obj_remove_state(row, LV_STATE_DISABLED);   /* for the key handler */
    else        lv_obj_add_state(row, LV_STATE_DISABLED);
    /* dim the whole row (icon + label + chevron) — the label's own text colour
     * would otherwise override an inherited disabled style. */
    lv_obj_set_style_opa(row, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
}

void colstack_ensure_escapable(colstack_t * cs)
{
    if(cs->depth <= 1) return;                       /* root is never a dead end   */
    int top = cs->depth - 1;
    if(lv_group_get_obj_count(cs->cols[top].group) > 0) return;   /* already navigable */

    /* An inert, zero-footprint focusable row: the group auto-focuses it (it is the
     * only member), so the Back key lands on item_key_cb and pops the column. The
     * page's own "none found" caption stays the visible content; this is invisible.
     * NOTE: it must NOT be hidden or disabled — focus_next_core skips both. */
    lv_obj_t * row = lv_obj_create(cs->cols[top].body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 0, 0);

    cs_item_t * it = calloc(1, sizeof(cs_item_t));
    it->cs   = cs;
    it->role = CS_ACTION;                            /* forward keys inert; Back pops */
    lv_obj_set_user_data(row, it);

    lv_group_add_obj(cs->cols[top].group, row);
    lv_obj_add_event_cb(row, item_key_cb, LV_EVENT_KEY, it);
    lv_obj_add_event_cb(row, item_delete_cb, LV_EVENT_DELETE, it);
}

/* Size the current (top) column to fit its widest row, clamped to [MIN,MAX].
 * Labels wrap only when they'd exceed MAX — so short pages stay compact and
 * long labels (e.g. verbose slider descriptions) get a wider column instead of
 * being squeezed. */
#define CS_COL_MIN 320
#define CS_COL_MAX 720

static int32_t label_text_w(lv_obj_t * label)
{
    if(!label) return 0;
    const lv_font_t * font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(label), font,
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

static int32_t role_ctrl_w(const cs_item_t * it)
{
    switch(it->role) {
        case CS_SLIDER:   return label_text_w(it->slider_val) + 120 /*bar*/ + 60;
        case CS_DROPDOWN: {             /* value (capped) + chevron + gaps      */
            int32_t vw = label_text_w(it->value_lbl);
            if(vw > 380) vw = 380;
            return vw + 48;
        }
        case CS_SWITCH:   return 70;
        case CS_SUBMENU:  return 34;    /* chevron                              */
        default:          return 20;
    }
}

void colstack_autofit(colstack_t * cs)
{
    int top = cs->depth - 1;
    lv_obj_t * body = cs->cols[top].body;
    int32_t need = CS_COL_MIN;

    uint32_t n = lv_obj_get_child_count(body);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t * ch = lv_obj_get_child(body, i);
        int32_t w;
        /* +SLACK so the label always has a few px to spare and never wraps a
         * single trailing character. */
        const int32_t SLACK = 32;
        if(lv_obj_check_type(ch, &lv_label_class)) {      /* a caption/title row */
            w = label_text_w(ch) + 24 + SLACK;
        } else {
            cs_item_t * it = lv_obj_get_user_data(ch);
            if(!it) {
                /* not a colstack row (e.g. an emit_value info card): sum its
                 * label children so a long value/SSID still widens the column. */
                int32_t sum = 0;
                uint32_t m = lv_obj_get_child_count(ch);
                for(uint32_t j = 0; j < m; j++) {
                    lv_obj_t * gc = lv_obj_get_child(ch, j);
                    if(lv_obj_check_type(gc, &lv_label_class)) sum += label_text_w(gc) + 8;
                }
                if(!sum) continue;
                w = sum + 24 /*pad*/ + SLACK;
            } else {
                /* ACTION rows (including TEXT items) may have a value label child.
                 * For TEXT items, this holds the current value (e.g., SSID or password).
                 * If present, sum the value width instead of guessing. */
                int32_t ctrl = role_ctrl_w(it);
                if(it->role == CS_ACTION) {
                    int32_t sum = 0;
                    uint32_t m = lv_obj_get_child_count(ch);
                    for(uint32_t j = 0; j < m; j++) {
                        lv_obj_t * gc = lv_obj_get_child(ch, j);
                        if(lv_obj_check_type(gc, &lv_label_class)) sum += label_text_w(gc) + 8;
                    }
                    if(sum > 0) ctrl = sum;
                }
                w = label_text_w(it->label) + ctrl + 34 /*icon*/ + 24 /*pad*/ + SLACK;
            }
        }
        if(w > need) need = w;
    }
    if(need > CS_COL_MAX) need = CS_COL_MAX;
    lv_obj_set_width(cs->cols[top].col, need);
    /* Force the flex/label layout to settle at the new width now — otherwise a
     * LONG_WRAP label that wrapped at the old width keeps its stale line break
     * until some later event (e.g. moving a slider) re-flows it. */
    lv_obj_update_layout(cs->cols[top].col);
}

/* ── lifecycle ─────────────────────────────────────────────────────────────── */

colstack_t * colstack_create(lv_obj_t * parent)
{
    colstack_t * cs = calloc(1, sizeof(colstack_t));

    lv_obj_t * stack = lv_obj_create(parent);
    lv_obj_set_size(stack, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stack, 0, 0);
    lv_obj_set_style_pad_all(stack, 8, 0);
    lv_obj_set_style_pad_column(stack, 8, 0);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(stack, LV_DIR_HOR);
    /* keep the root at the left; scroll_to_view brings deeper columns into view
     * once the cascade grows wider than the viewport. */
    lv_obj_set_scroll_snap_x(stack, LV_SCROLL_SNAP_NONE);
    lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    cs->stack = stack;

    /* root column (always present) */
    lv_obj_t * col   = make_column(cs, "Menu");
    lv_obj_t * body  = lv_obj_get_user_data(col);
    lv_group_t * grp = lv_group_create();
    cs->cols[0].col = col;
    cs->cols[0].body = body;
    cs->cols[0].group = grp;
    cs->depth = 1;

    lv_indev_set_group(indev_drv, grp);
    refresh_active_styling(cs);
    return cs;
}
