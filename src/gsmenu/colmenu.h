#pragma once
/*
 * colmenu — native, descriptor-driven column menu (the target pattern).
 *
 * A page is DATA: a title, the gsmenu.sh (type,page) namespace, and a table of
 * items. One binder reads gsmenu.sh for the current value/options, renders the
 * column row, and on change writes gsmenu.sh + calls the item's domain hook.
 *
 * No lv_menu, no hidden widgets, no child-by-type pokes, no reload threads —
 * contrast with the imperative create_gs_system_display_menu() in gs_system.c.
 */
#include "../../lvgl/lvgl.h"

typedef enum {
    COLMENU_LABEL,      /* caption / read-only row               */
    COLMENU_VALUE,      /* read-only "label: <gsmenu.sh get>"    */
    COLMENU_SWITCH,     /* bound bool                            */
    COLMENU_SLIDER,     /* bound number (precision decimals)     */
    COLMENU_DROPDOWN,   /* bound enum → opens a radio column     */
    COLMENU_TEXT,       /* bound string → opens a textarea+kbd column */
    COLMENU_SUBMENU,    /* descends into another page            */
    COLMENU_ACTION,     /* fires on_activate                     */
} colmenu_kind_t;

struct colmenu_page;

/* RX mode bitmask for item gating: bit 0=WFB, bit 1=APFPV. 0=show in all modes. */
typedef enum {
    COLMENU_MODE_WFB   = (1 << 0),
    COLMENU_MODE_APFPV = (1 << 1),
    COLMENU_MODE_ALL   = (1 << 0) | (1 << 1),
} colmenu_mode_t;

typedef struct colmenu_item {
    colmenu_kind_t              kind;
    const char *                icon;
    const char *                label;
    const char *                param;      /* gsmenu.sh parameter               */
    int                         precision;  /* slider decimals                   */
    int                         display_scale; /* SLIDER: multiply displayed value by this (e.g., 100 for pos*100 MB) */
    colmenu_mode_t              mode_mask;  /* bitmask: which RX modes show this item (0=all, COLMENU_MODE_*) */
    const struct colmenu_page * sub;        /* COLMENU_SUBMENU target            */
    void (*on_change)(const char * value);  /* domain hook after a set (optional)*/
    void (*on_live)(const char * value);    /* SLIDER: fired live while dragging  */
    void (*on_activate)(void);              /* COLMENU_ACTION                    */
} colmenu_item_t;

/* Handle passed to a dynamic page's build() so it can emit rows at runtime. */
typedef struct colmenu_emit colmenu_emit_t;

typedef struct colmenu_page {
    const char *           title;
    const char *           type;   /* gsmenu.sh get/set <type> <page> ...        */
    const char *           page;
    const colmenu_item_t * items;  /* static pages: a table                      */
    int                    count;
    /* Dynamic pages: build the rows at runtime (e.g. from a scan). When set,
     * items/count are ignored and build() runs each time the page is opened. */
    void (*build)(colmenu_emit_t * e);
    /* Optional (dynamic pages only): NULL-terminated list of gsmenu.sh params to
     * warm on a worker thread behind a spinner before build() runs, so a slow read
     * (e.g. the WiFi scan) never freezes the UI. build()'s colmenu_get() calls for
     * these params are then served from the prefetch cache. */
    const char * const * prefetch;
} colmenu_page_t;

/* Create the column engine inside `parent`. */
void colmenu_init(lv_obj_t * parent);
/* Build `root` as the first column. */
void colmenu_show(const colmenu_page_t * root);

/* Reset to the root column and rebuild it (e.g. after a mode switch changes the
 * page structure). */
void colmenu_rebuild(void);

/* Drone (VTX) detection: gates the air pages (greyed when not detected). */
void colmenu_set_drone_detected(bool detected);
bool colmenu_drone_detected(void);

/* ── emit API for dynamic build() functions ────────────────────────────────── */
void colmenu_emit_label  (colmenu_emit_t * e, const char * text);
void colmenu_emit_value  (colmenu_emit_t * e, const char * icon, const char * label, const char * value);
/* A bound switch in a dynamic page: reads gsmenu.sh get <type> <page> <param> for
 * the initial state and writes on/off on toggle (type/page/param are copied). */
void colmenu_emit_switch (colmenu_emit_t * e, const char * icon, const char * label,
                          const char * type, const char * page, const char * param,
                          void (*on_change)(const char * value));
void colmenu_emit_submenu(colmenu_emit_t * e, const char * icon, const char * label, const colmenu_page_t * sub);
/* Like emit_submenu, but the row is gated on drone (VTX) detection — greyed and
 * un-enterable until colmenu_set_drone_detected(true). */
void colmenu_emit_submenu_gated(colmenu_emit_t * e, const char * icon, const char * label, const colmenu_page_t * sub);
/* ctx is passed to on_activate and free()'d when the row is destroyed. */
void colmenu_emit_action (colmenu_emit_t * e, const char * icon, const char * label,
                          void (*on_activate)(void * ctx), void * ctx);

/* Re-run the current dynamic column's builder (e.g. a wifi re-scan). Deferred. */
void colmenu_rescan(void);

/* Reflect an external state change onto a bound switch (found by gsmenu.sh param)
 * without firing its on_change — e.g. a hardware button toggling DVR recording
 * should move the "Enabled" switch if that page is open. No-op if not shown. */
void colmenu_reflect_switch(const char * param, bool on);

/* Open a floating on-screen-keyboard prompt; on OK, `on_ok` gets the typed text
 * (nothing is written to gsmenu.sh). For e.g. a wifi password. */
void colmenu_prompt(const char * title, void (*on_ok)(const char * text, void * user), void * user);

/* Run a shell command on a worker thread while a spinner blocks the menu (like the
 * old menu did for writes). stdout/stderr and the exit code are captured; a
 * non-zero exit shows an error dialog with the command output. */
void colmenu_exec(const char * cmd);

/* Integration hooks for embedding in the app's menu screen. */
lv_group_t * colmenu_root_group(void);                               /* focus on open */
void         colmenu_set_root_back(void (*cb)(void * ctx), void * ctx); /* Back at root */

/* Read gsmenu.sh directly (for dynamic builders): malloc'd value or NULL;
 * *opts (optional) = malloc'd blob after the 0x1e separator. */
char * colmenu_get(const char * type, const char * page, const char * param, char ** opts);
