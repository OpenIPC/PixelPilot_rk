#pragma once
/*
 * column_stack — a cascading "Miller column" navigation container for LVGL.
 *
 * Instead of lv_menu's single-visible-page model (sidebar + one swapped main
 * page), the column stack keeps every level visible side by side:
 *
 *     [ root column | page A | sub-page B | dropdown list C ]
 *
 * RIGHT/ENTER on a row that owns a sub-level pushes a new column to the right
 * (the existing columns stay put); LEFT/ESC pops the right-most column. Each
 * column owns its own input group, so focus and the key handler always target
 * the visible right-most column.
 *
 * This is a self-contained widget (plain lv_obj containers + groups); it does
 * NOT subclass or depend on lv_menu.
 */

#include "../../lvgl/lvgl.h"

typedef struct colstack colstack_t;

/* Builder callback: fill `col_body` with the rows for a freshly pushed column.
 * `user` is the pointer passed to colstack_add_submenu(). */
typedef void (*colstack_builder_t)(colstack_t * cs, lv_obj_t * col_body, void * user);

/* Create the horizontal stack inside `parent` and its first (root) column.
 * The stack takes the full size of `parent`. */
colstack_t * colstack_create(lv_obj_t * parent);

/* Body of the root column — add your top-level rows here. */
lv_obj_t * colstack_root(colstack_t * cs);

/* Push an empty column titled `title`, make it the active column (its group
 * becomes the indev group) and scroll it into view. Returns the column body.
 * Normally you don't call this directly — the row helpers below do. */
lv_obj_t * colstack_push(colstack_t * cs, const char * title);

/* Pop the right-most column (no-op on the root column). */
void colstack_pop(colstack_t * cs);

int colstack_depth(colstack_t * cs);

/* Input group of the active (right-most) column — the shim adds reparented
 * rows to this so keys reach them. */
lv_group_t * colstack_cur_group(colstack_t * cs);

/* ── Row helpers ─────────────────────────────────────────────────────────────
 * These mirror the shape of the existing create_* helpers in helper.c so the
 * page-builder files barely change when ported. Each returns the row object. */

/* A row that descends into a sub-column built lazily by `builder`. */
lv_obj_t * colstack_add_submenu(colstack_t * cs, lv_obj_t * col, const char * icon,
                                const char * text, colstack_builder_t builder, void * user);

/* A plain inline toggle (like lv_menu switch). */
lv_obj_t * colstack_add_switch(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text, bool on);

/* An inline slider edited in place: ENTER toggles edit mode, LEFT/RIGHT adjust.
 * min/max/val are integers scaled by 10^precision; the label and the value
 * reported to a binding are formatted with `precision` decimals.
 * display_scale: multiply the displayed value by this (e.g., 100 for slider_pos*100 MB). */
lv_obj_t * colstack_add_slider(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text, int32_t min, int32_t max, int32_t val,
                               int precision, int display_scale);

/* A dropdown that opens a radio list in a NEW column (the requested behaviour).
 * `options` is a '\n'-separated list; `sel` is the initially selected index. */
lv_obj_t * colstack_add_dropdown(colstack_t * cs, lv_obj_t * col, const char * icon,
                                 const char * text, const char * options, int sel);

/* A non-descending action row (fires a click; shown for completeness). */
lv_obj_t * colstack_add_action(colstack_t * cs, lv_obj_t * col, const char * icon,
                               const char * text);

/* Optional: a section-title label row. */
lv_obj_t * colstack_add_title(colstack_t * cs, lv_obj_t * col, const char * text);

/* Attach a value binding to a switch/slider/dropdown row: `on_change` is called
 * with the new value string ("1"/"0" for a switch, a formatted number for a
 * slider, the chosen option text for a dropdown) whenever the user changes it. */
void colstack_set_binding(lv_obj_t * row, void (*on_change)(void * ctx, const char * value),
                          void * ctx);

/* Tag a row with its gsmenu.sh param so its switch can be found and updated by
 * an external state change (see colstack_reflect_switch). */
void colstack_set_row_param(lv_obj_t * row, const char * param);

/* Set the visual state of a bound switch (found by `param`) in any visible
 * column, WITHOUT firing its binding — for state changed outside the menu
 * (e.g. a hardware button toggling DVR recording). No-op if not shown. */
void colstack_reflect_switch(colstack_t * cs, const char * param, bool on);

/* Attach a "live" callback to a slider row: fired with the formatted value on
 * every adjustment while editing, and again with the original value if the edit
 * is cancelled — for live-preview effects (e.g. DRM video scaling). */
void colstack_set_slider_live(lv_obj_t * row, void (*on_live)(void * ctx, const char * value),
                              void * ctx);

/* Attach an activation callback to an action row (fired on the ENTER key press). */
void colstack_set_action(lv_obj_t * row, void (*on_activate)(void * ctx), void * ctx);

/* Called when Back is pressed at the root column (depth 1) — e.g. close the menu. */
void colstack_set_root_back(colstack_t * cs, void (*cb)(void * ctx), void * ctx);
/* Input group of the root column (for the app to focus when opening the menu). */
lv_group_t * colstack_root_group(colstack_t * cs);

/* Enable/disable a row: disabled rows are greyed and can't be entered/activated
 * (Back still works), used to gate the drone pages on VTX detection. */
void colstack_set_enabled(lv_obj_t * row, bool enabled);

/* The row that opened column `idx` (idx 1 = first pushed from the root). */
lv_obj_t * colstack_column_origin(colstack_t * cs, int idx);

/* Re-run the top column's builder into a cleared body (refresh a dynamic list). */
void colstack_rebuild_current(colstack_t * cs);

/* Resize the current (top) column to fit its widest row (clamped). Call after
 * a column has been fully populated. */
void colstack_autofit(colstack_t * cs);

/* Guarantee the top (non-root) column can be left with Back. A dynamic page that
 * emits only captions (e.g. an empty list showing just a "none found" note) leaves
 * the column's input group with no focusable row — so no object receives the Back
 * key and the user is stranded. Call after a column is populated: if the group is
 * empty it drops in an inert, invisible focusable catcher so Back still pops. */
void colstack_ensure_escapable(colstack_t * cs);
