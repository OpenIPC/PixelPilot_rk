#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ui.h"
#include "../input.h"
#include "../gstrtpreceiver.h"
#include "helper.h"
#include "styles.h"
#include "executor.h"
#include "gs_wifi.h"

/* ── Network data ────────────────────────────────────────────────────────── */

#define MAX_NETWORKS 64
#define MAX_SSID_LEN 64

typedef struct {
    char ssid[MAX_SSID_LEN];
    char security[64];   /* empty string = open network */
    int  signal;
    char saved_password[128]; /* non-empty = remembered password */
} wifi_network_t;

static wifi_network_t networks[MAX_NETWORKS];
static int            network_count = 0;
static int            selected_network_idx = -1;

/* ── Async scan state ────────────────────────────────────────────────────── */

typedef struct {
    char           * output;
    volatile bool    complete;
    bool             running;
    pthread_t        thread;
    lv_obj_t       * spinner;
} scan_state_t;

static scan_state_t scan_state = {0};

/* ── External references ─────────────────────────────────────────────────── */

extern lv_obj_t            * menu;
extern gsmenu_control_mode_t control_mode;
extern lv_group_t          * default_group;
extern lv_indev_t          * indev_drv;

/* ── Widget references ───────────────────────────────────────────────────── */

static lv_obj_t        * hotspot;
static lv_obj_t        * net_list_cont;   /* dynamic list container          */
static lv_obj_t        * pwd_row;         /* password row, hidden by default */
static lv_obj_t        * pwd_ta;          /* password textarea               */
static lv_obj_t        * wifi_kb;         /* on-screen keyboard              */
static lv_obj_t        * status_lbl;      /* connection status               */
static lv_obj_t        * ipinfo;
static lv_obj_t        * ipinfo_label_ref; /* cached ref for group reordering */
static lv_obj_t        * hotspot_sw_ref;   /* cached ref for group reordering */
static lv_obj_t        * restream_sw_ref;  /* cached ref for group reordering */
static lv_obj_t        * restream;
static lv_obj_t        * ip_dropdown;
static menu_page_data_t* wifi_mpd;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void parse_scan_output(char * output);
static void load_saved_passwords(void);
static void rebuild_network_list(void);
static void update_status(void);
static void scan_btn_cb(lv_event_t * e);
static void connect_network_btn_cb(lv_event_t * e);
static void disconnect_btn_cb(lv_event_t * e);
static void pwd_kb_cb(lv_event_t * e);
static void after_connect(void);
static void after_disconnect(void);
static void hotspot_switch_status_cb(lv_event_t * e);
static void scan_complete_timer_cb(lv_timer_t * t);

/* ── Restream helpers  ─────────────────────────────────────────────────── */

static uint16_t find_dropdown_option_index(const char * options, const char * value)
{
    if (!value || value[0] == '\0') return 0;
    uint16_t idx = 0;
    const char * option = options;
    while (option && *option) {
        const char * nl = strchr(option, '\n');
        size_t len = nl ? (size_t)(nl - option) : strlen(option);
        if (strlen(value) == len && strncmp(option, value, len) == 0) return idx;
        idx++;
        option = nl ? nl + 1 : NULL;
    }
    return 0;
}

void wifi_page_load_callback(lv_obj_t * page)
{
    reload_switch_value(page,hotspot);
    reload_switch_value(page,wlan);
    reload_textarea_value(page,ssid);
    reload_textarea_value(page,password);
    reload_label_value(page,ipinfo);

#ifndef USE_SIMULATOR
    if (restream_get_enabled()) lv_obj_add_state(lv_obj_get_child_by_type(restream,0,&lv_switch_class), LV_STATE_CHECKED);
    else lv_obj_clear_state(lv_obj_get_child_by_type(restream,0,&lv_switch_class), LV_STATE_CHECKED);
    {
        char clients[512];
        restream_scan_clients(clients, sizeof(clients));
        lv_dropdown_set_options(ip_dropdown, clients);
        lv_dropdown_set_selected(ip_dropdown, find_dropdown_option_index(clients, restream_get_manual_ip()));
    }
#endif
}
static void ip_dropdown_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        char buf[64];
        lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
#ifndef USE_SIMULATOR
        restream_set_manual_ip(buf);
#endif
    }
}

static void restream_switch_callback(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * target = lv_event_get_target(e);
#ifndef USE_SIMULATOR
        restream_set_enabled(lv_obj_has_state(target, LV_STATE_CHECKED));
#endif
    }
}

/* ── Parse iw scan output ────────────────────────────────────────────────── */
/*
 * Each line from: gsmenu.sh get gs wifi networks  (iw dev wlan0 scan | awk)
 *   ESCAPED_SSID:SECURITY:SIGNAL_PCT
 * where literal ':' inside SSID is escaped as '\:'.
 * SECURITY is "--" for open networks, "WPA" for secured.
 */
static void parse_scan_output(char * output) {
    network_count = 0;
    if (!output || output[0] == '\0') return;

    char * saveptr = NULL;
    char * line = strtok_r(output, "\n", &saveptr);

    while (line && network_count < MAX_NETWORKS) {
        if (line[0] == '\0') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        int len = (int)strlen(line);

        /* Find the last two unescaped colons (not preceded by backslash) */
        int last_colon = -1, second_last_colon = -1;
        for (int i = 0; i < len; i++) {
            if (line[i] == ':' && (i == 0 || line[i - 1] != '\\')) {
                second_last_colon = last_colon;
                last_colon = i;
            }
        }
        if (last_colon < 0 || second_last_colon < 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        char * signal_str = line + last_colon + 1;
        line[last_colon] = '\0';
        char * security = line + second_last_colon + 1;
        line[second_last_colon] = '\0';
        char * ssid_raw = line;

        /* Unescape \: → : in-place */
        int si = 0, di = 0;
        while (ssid_raw[si]) {
            if (ssid_raw[si] == '\\' && ssid_raw[si + 1] == ':') {
                ssid_raw[di++] = ':';
                si += 2;
            } else {
                ssid_raw[di++] = ssid_raw[si++];
            }
        }
        ssid_raw[di] = '\0';

        if (ssid_raw[0] == '\0') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        /* Deduplicate by SSID (keep first = strongest, nmcli sorts by signal) */
        bool seen = false;
        for (int j = 0; j < network_count; j++) {
            if (strcmp(networks[j].ssid, ssid_raw) == 0) { seen = true; break; }
        }
        if (seen) { line = strtok_r(NULL, "\n", &saveptr); continue; }

        strncpy(networks[network_count].ssid, ssid_raw, MAX_SSID_LEN - 1);
        networks[network_count].ssid[MAX_SSID_LEN - 1] = '\0';
        strncpy(networks[network_count].security, security,
                sizeof(networks[0].security) - 1);
        networks[network_count].security[sizeof(networks[0].security) - 1] = '\0';
        networks[network_count].signal = atoi(signal_str);
        network_count++;

        line = strtok_r(NULL, "\n", &saveptr);
    }
}

/* ── Async scan ──────────────────────────────────────────────────────────── */

static void * scan_thread_fn(void * arg) {
    (void)arg;
    scan_state.output = run_command("gsmenu.sh get gs wifi networks");
    scan_state.complete = true;
    return NULL;
}

static void scan_complete_timer_cb(lv_timer_t * t) {
    if (!scan_state.complete) return;

    pthread_join(scan_state.thread, NULL);

    if (scan_state.spinner && lv_obj_is_valid(scan_state.spinner)) {
        lv_obj_del(scan_state.spinner);
        scan_state.spinner = NULL;
    }
    lv_timer_del(t);
    scan_state.running = false;

    parse_scan_output(scan_state.output);
    free(scan_state.output);
    scan_state.output = NULL;
    scan_state.complete = false;

    rebuild_network_list();

    /* Restore input group */
    if (wifi_mpd) lv_indev_set_group(indev_drv, wifi_mpd->indev_group);
}

/* ── Saved password store ───────────────────────────────────────────────── */

static void load_saved_passwords(void) {
    int i;
    for (i = 0; i < network_count; i++)
        networks[i].saved_password[0] = '\0';

    char * output = run_command("gsmenu.sh get gs wifi savednetworks");
    if (!output || output[0] == '\0') { free(output); return; }

    char * saveptr = NULL;
    char * line = strtok_r(output, "\n", &saveptr);
    while (line) {
        if (line[0] == '\0') { line = strtok_r(NULL, "\n", &saveptr); continue; }
        int len = (int)strlen(line);
        /* Find first unescaped colon — separator between SSID and password */
        int colon = -1;
        for (int j = 0; j < len; j++) {
            if (line[j] == ':' && (j == 0 || line[j-1] != '\\')) { colon = j; break; }
        }
        if (colon < 0) { line = strtok_r(NULL, "\n", &saveptr); continue; }
        char * password = line + colon + 1;
        line[colon] = '\0';
        /* Unescape \: -> : in SSID */
        int si = 0, di = 0;
        while (line[si]) {
            if (line[si] == '\\' && line[si+1] == ':') { line[di++] = ':'; si += 2; }
            else { line[di++] = line[si++]; }
        }
        line[di] = '\0';
        /* Match to scanned networks */
        for (i = 0; i < network_count; i++) {
            if (strcmp(networks[i].ssid, line) == 0) {
                strncpy(networks[i].saved_password, password,
                        sizeof(networks[i].saved_password) - 1);
                networks[i].saved_password[sizeof(networks[i].saved_password) - 1] = '\0';
                break;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(output);
}

/* ── Network list ────────────────────────────────────────────────────────── */

static void rebuild_network_list(void) {
    load_saved_passwords();
    /* Remove all below-list objects from group so network buttons are inserted
       immediately after scan/disconnect in the correct nav order. */
    if (hotspot_sw_ref && lv_obj_is_valid(hotspot_sw_ref))
        lv_group_remove_obj(hotspot_sw_ref);
    if (restream_sw_ref && lv_obj_is_valid(restream_sw_ref))
        lv_group_remove_obj(restream_sw_ref);
    lv_group_remove_obj(ip_dropdown);
    if (ipinfo_label_ref && lv_obj_is_valid(ipinfo_label_ref))
        lv_group_remove_obj(ipinfo_label_ref);

    /* lv_obj_clean auto-removes any deleted Connect buttons from all groups */
    lv_obj_clean(net_list_cont);

    if (network_count == 0) {
        lv_obj_t * lbl = lv_label_create(net_list_cont);
        lv_label_set_text(lbl, "No networks found");
    } else {
        /* Fetch currently connected SSID to mark it.
         * Skip when hotspot is active — the interface is in AP mode. */
        char connected_ssid[MAX_SSID_LEN] = "";
        bool hotspot_on = (hotspot_sw_ref && lv_obj_is_valid(hotspot_sw_ref) &&
                           lv_obj_has_state(hotspot_sw_ref, LV_STATE_CHECKED));
        if (!hotspot_on) {
            char * conn = run_command("gsmenu.sh get gs wifi ssid");
            if (conn) {
                size_t len = strlen(conn);
                while (len > 0 && (conn[len-1] == '\n' || conn[len-1] == '\r'))
                    conn[--len] = '\0';
                if (len > 0 && len < MAX_SSID_LEN)
                    strncpy(connected_ssid, conn, MAX_SSID_LEN - 1);
                free(conn);
            }
        }

        for (int i = 0; i < network_count; i++) {
            bool is_connected = (connected_ssid[0] != '\0' &&
                                 strcmp(networks[i].ssid, connected_ssid) == 0);
            bool is_open = (networks[i].security[0] == '\0' ||
                            strcmp(networks[i].security, "--") == 0);

            /* One full-width button per network — no nested rows, no nav issues */
            lv_obj_t * btn = lv_btn_create(net_list_cont);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_obj_set_height(btn, LV_SIZE_CONTENT);
            lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(btn, 6, 0);
            lv_obj_set_style_pad_column(btn, 8, 0);

            if (is_connected) {
                lv_obj_add_style(btn, &style_openipc_dark_background,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
            } else {
                lv_obj_add_style(btn, &style_openipc,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
            }
            lv_obj_add_style(btn, &style_openipc_outline,
                             LV_PART_MAIN | LV_STATE_FOCUS_KEY);
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, connect_network_btn_cb,
                                LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(btn, generic_back_event_handler,
                                LV_EVENT_KEY, NULL);
            lv_obj_add_event_cb(btn, on_focus, LV_EVENT_FOCUSED, NULL);
            lv_group_add_obj(wifi_mpd->indev_group, btn);

            lv_obj_t * icon = lv_label_create(btn);
            bool has_saved = (networks[i].saved_password[0] != '\0');
            lv_label_set_text(icon, is_open ? LV_SYMBOL_WIFI :
                                    (has_saved ? LV_SYMBOL_OK : LV_SYMBOL_WARNING));

            lv_obj_t * name = lv_label_create(btn);
            char name_buf[MAX_SSID_LEN + 16];
            snprintf(name_buf, sizeof(name_buf), "%s (%d%%)",
                     networks[i].ssid, networks[i].signal);
            lv_label_set_text(name, name_buf);
            lv_obj_set_flex_grow(name, 1);

            lv_obj_t * state_lbl = lv_label_create(btn);
            lv_label_set_text(state_lbl,
                              is_connected ? (LV_SYMBOL_OK " Connected") : ">");
        }
    }

    /* Restore in correct nav order: hotspot, restream, ip_dropdown, ipinfo */
    if (hotspot_sw_ref && lv_obj_is_valid(hotspot_sw_ref))
        lv_group_add_obj(wifi_mpd->indev_group, hotspot_sw_ref);
    if (restream_sw_ref && lv_obj_is_valid(restream_sw_ref))
        lv_group_add_obj(wifi_mpd->indev_group, restream_sw_ref);
    lv_group_add_obj(wifi_mpd->indev_group, ip_dropdown);
    if (ipinfo_label_ref && lv_obj_is_valid(ipinfo_label_ref))
        lv_group_add_obj(wifi_mpd->indev_group, ipinfo_label_ref);
}

/* ── Status ──────────────────────────────────────────────────────────────── */

static void update_status(void) {
    /* When hotspot is active the interface is in AP mode — not a client.
     * Read from the already-loaded switch widget to avoid a redundant
     * gsmenu.sh call on page load (reload_switch_value runs first). */
    if (hotspot_sw_ref && lv_obj_is_valid(hotspot_sw_ref) &&
        lv_obj_has_state(hotspot_sw_ref, LV_STATE_CHECKED)) {
        lv_label_set_text(status_lbl, LV_SYMBOL_WIFI " Hotspot active");
        return;
    }
    char * conn = run_command("gsmenu.sh get gs wifi ssid");
    if (conn) {
        size_t len = strlen(conn);
        if (len > 0 && conn[len - 1] == '\n') conn[len - 1] = '\0';
        if (conn[0] != '\0') {
            char buf[256];
            snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connected: %s", conn);
            lv_label_set_text(status_lbl, buf);
        } else {
            lv_label_set_text(status_lbl, "Not connected");
        }
        free(conn);
    } else {
        lv_label_set_text(status_lbl, "Not connected");
    }
}

/* ── After-action callbacks ──────────────────────────────────────────────── */

static void after_connect(void) {
    lv_obj_add_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(pwd_ta, "");
    /* Connecting to WiFi means hotspot is now off — update switch visually */
    if (hotspot_sw_ref && lv_obj_is_valid(hotspot_sw_ref))
        lv_obj_clear_state(hotspot_sw_ref, LV_STATE_CHECKED);
    update_status();
    if (network_count > 0)
        rebuild_network_list(); /* refresh key icons after saving new password */
}

static void after_disconnect(void) {
    update_status();
}

static void hotspot_switch_status_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t * sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        lv_label_set_text(status_lbl, LV_SYMBOL_WIFI " Hotspot active");
    } else {
        lv_label_set_text(status_lbl, "Not connected");
    }
    /* Refresh the network list so no network remains marked "Connected"
     * after hotspot is toggled on, and vice versa. */
    if (network_count > 0)
        rebuild_network_list();
}

/* ── Page load callback ──────────────────────────────────────────────────── */

void wifi_page_load_callback(lv_obj_t * page)
{
    reload_switch_value(page, hotspot);
    update_status();
    reload_label_value(page, ipinfo);

    lv_obj_add_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);

    if (restream_get_enabled())
        lv_obj_add_state(lv_obj_get_child_by_type(restream, 0, &lv_switch_class),
                         LV_STATE_CHECKED);
    else
        lv_obj_clear_state(lv_obj_get_child_by_type(restream, 0, &lv_switch_class),
                           LV_STATE_CHECKED);
    {
        char clients[512];
        restream_scan_clients(clients, sizeof(clients));
        lv_dropdown_set_options(ip_dropdown, clients);
        lv_dropdown_set_selected(ip_dropdown,
            find_dropdown_option_index(clients, restream_get_manual_ip()));
    }
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void scan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (scan_state.running) return;

    scan_state.running  = true;
    scan_state.complete = false;

    lv_indev_set_group(indev_drv, default_group);

    scan_state.spinner = lv_spinner_create(lv_layer_top());
    lv_obj_add_style(scan_state.spinner, &style_openipc,
                     LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_center(scan_state.spinner);

    pthread_create(&scan_state.thread, NULL, scan_thread_fn, NULL);
    lv_timer_create(scan_complete_timer_cb, 30, NULL);
}

static void connect_network_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t * btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= network_count) return;

    selected_network_idx = idx;

    bool is_open = (networks[idx].security[0] == '\0'
                    || strcmp(networks[idx].security, "--") == 0);
    bool has_saved = (networks[idx].saved_password[0] != '\0');

    if (is_open || has_saved) {
        char cmd[512];
        if (is_open) {
            snprintf(cmd, sizeof(cmd),
                     "gsmenu.sh set gs wifi connect \"%s\"", networks[idx].ssid);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "gsmenu.sh set gs wifi connect \"%s\" \"%s\"",
                     networks[idx].ssid, networks[idx].saved_password);
        }
        run_command_and_block(e, cmd, after_connect);
    } else {
        lv_obj_clear_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(pwd_ta, "");
        lv_keyboard_set_textarea(wifi_kb, pwd_ta);
        lv_obj_scroll_to_view_recursive(pwd_row, LV_ANIM_OFF);
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
        lv_group_focus_obj(wifi_kb);
    }
}

static void disconnect_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    run_command_and_block(e, "gsmenu.sh set gs wifi disconnect", after_disconnect);
}

static void pwd_kb_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_FOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_KEYBOARD;
    } else if (code == LV_EVENT_DEFOCUSED) {
        control_mode = GSMENU_CONTROL_MODE_NAV;
    } else if (code == LV_EVENT_READY) {
        if (selected_network_idx >= 0 && selected_network_idx < network_count) {
            const char * pwd = lv_textarea_get_text(pwd_ta);
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "gsmenu.sh set gs wifi connect \"%s\" \"%s\"",
                     networks[selected_network_idx].ssid, pwd);
            lv_obj_add_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
            control_mode = GSMENU_CONTROL_MODE_NAV;
            run_command_and_block(e, cmd, after_connect);
        }
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(pwd_ta, "");
        control_mode = GSMENU_CONTROL_MODE_NAV;
        lv_indev_reset(NULL, lv_event_get_target(e));
    }
}

/* ── Create menu ─────────────────────────────────────────────────────────── */

void create_wifi_menu(lv_obj_t * parent) {

    menu_page_data_t * menu_page_data = malloc(sizeof(menu_page_data_t));
    memset(menu_page_data, 0, sizeof(menu_page_data_t));
    strcpy(menu_page_data->type, "gs");
    strcpy(menu_page_data->page, "wifi");
    menu_page_data->page_load_callback = wifi_page_load_callback;
    menu_page_data->indev_group = lv_group_create();
    lv_group_set_default(menu_page_data->indev_group);
    lv_obj_set_user_data(parent, menu_page_data);
    wifi_mpd = menu_page_data;

    /* ── WiFi Connection ─────────────────────────────────────────────────── */

    create_text(parent, NULL, "WiFi Connection", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t * section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);

    lv_obj_t * cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    /* Top bar: [Scan] [Status] [Disconnect] */
    lv_obj_t * top_row = lv_menu_cont_create(cont);

    lv_obj_t * scan_btn = lv_btn_create(top_row);
    lv_obj_t * scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_add_style(scan_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(scan_btn, &style_openipc_outline,
                     LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scan_btn, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(scan_btn, on_focus, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(menu_page_data->indev_group, scan_btn);

    status_lbl = lv_label_create(top_row);
    lv_label_set_text(status_lbl, "Not connected");
    lv_obj_set_flex_grow(status_lbl, 1);

    lv_obj_t * disc_btn = lv_btn_create(top_row);
    lv_obj_t * disc_lbl = lv_label_create(disc_btn);
    lv_label_set_text(disc_lbl, LV_SYMBOL_CLOSE " Disconnect");
    lv_obj_add_style(disc_btn, &style_openipc_dark_background,
                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(disc_btn, &style_openipc_outline,
                     LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(disc_btn, disconnect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(disc_btn, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(disc_btn, on_focus, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(menu_page_data->indev_group, disc_btn);

    /* Network list — populated dynamically after scan */
    net_list_cont = lv_menu_cont_create(cont);
    lv_obj_set_flex_flow(net_list_cont, LV_FLEX_FLOW_COLUMN);
    {
        lv_obj_t * hint = lv_label_create(net_list_cont);
        lv_label_set_text(hint, "Press Scan to search for networks");
    }

    /* Password row (hidden until a secured network is selected) */
    pwd_row = lv_menu_cont_create(cont);
    lv_obj_add_flag(pwd_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * pwd_label = lv_label_create(pwd_row);
    lv_label_set_text(pwd_label, LV_SYMBOL_EYE_CLOSE " Password");
    pwd_ta = lv_textarea_create(pwd_row);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_textarea_set_password_mode(pwd_ta, false);
    lv_textarea_set_placeholder_text(pwd_ta, "Enter password...");
    lv_obj_set_flex_grow(pwd_ta, 1);
    lv_obj_add_style(pwd_ta, &style_openipc_dark_background,
                     LV_PART_MAIN | LV_STATE_DEFAULT);

    /* On-screen keyboard (hidden by default) */
    wifi_kb = lv_keyboard_create(lv_obj_get_parent(section));
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_style(wifi_kb, &style_openipc_outline,
                     LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(wifi_kb, &style_openipc,
                     LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(wifi_kb, &style_openipc_dark_background,
                     LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_add_style(wifi_kb, &style_openipc_textcolor,
                     LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(wifi_kb, &style_openipc_lightdark_background,
                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(wifi_kb, pwd_kb_cb, LV_EVENT_ALL, NULL);
    lv_keyboard_set_textarea(wifi_kb, NULL);

    /* ── Hotspot ─────────────────────────────────────────────────────────── */

    create_text(parent, NULL, "Hotspot", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    hotspot = create_switch(cont, NULL, "Hotspot", "hotspot", menu_page_data, false);
    hotspot_sw_ref = lv_obj_get_child_by_type(hotspot, 0, &lv_switch_class);
    lv_obj_add_event_cb(hotspot_sw_ref, hotspot_switch_status_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Phone Restream ──────────────────────────────────────────────────── */

    create_text(parent, NULL, "Phone Restream", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    restream = create_switch(cont, LV_SYMBOL_VIDEO, "Phone Restream", NULL, NULL, false);
    restream_sw_ref = lv_obj_get_child_by_type(restream, 0, &lv_switch_class);
    lv_obj_add_event_cb(lv_obj_get_child_by_type(restream, 0, &lv_switch_class),
                        restream_switch_callback, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * ip_dropdown_row = lv_menu_cont_create(cont);
    lv_obj_t * dd_icon = lv_image_create(ip_dropdown_row);
    lv_image_set_src(dd_icon, LV_SYMBOL_WIFI);
    lv_obj_t * ip_dd_label = lv_label_create(ip_dropdown_row);
    lv_label_set_text(ip_dd_label, "Stream To");
    lv_obj_set_flex_grow(ip_dd_label, 1);
    ip_dropdown = lv_dropdown_create(ip_dropdown_row);
    lv_dropdown_set_options(ip_dropdown, "Auto");
    lv_dropdown_set_dir(ip_dropdown, LV_DIR_RIGHT);
    lv_dropdown_set_symbol(ip_dropdown, LV_SYMBOL_RIGHT);
    lv_obj_set_width(ip_dropdown, 200);
    lv_obj_add_style(ip_dropdown, &style_openipc_outline,
                     LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(ip_dropdown, &style_openipc_dark_background,
                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t * ip_dd_list = lv_dropdown_get_list(ip_dropdown);
    lv_obj_add_style(ip_dd_list, &style_openipc,
                     LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_style(ip_dd_list, &style_openipc_dark_background,
                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ip_dropdown, dropdown_event_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ip_dropdown, ip_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ip_dropdown, generic_back_event_handler, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(ip_dropdown, on_focus, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(menu_page_data->indev_group, ip_dropdown);

    /* ── Network info ────────────────────────────────────────────────────── */

    create_text(parent, NULL, "Network", NULL, NULL, false,
                LV_MENU_ITEM_BUILDER_VARIANT_1);
    section = lv_menu_section_create(parent);
    lv_obj_add_style(section, &style_openipc_section, 0);
    cont = lv_menu_cont_create(section);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

    ipinfo = create_text(cont, LV_SYMBOL_SETTINGS, "Network", "IP",
                         menu_page_data, false, LV_MENU_ITEM_BUILDER_VARIANT_1);
    lv_obj_t * ipinfo_label = lv_obj_get_child_by_type(ipinfo, 0, &lv_label_class);
    ipinfo_label_ref = ipinfo_label;   /* cache for group reordering in rebuild_network_list */
    lv_group_add_obj(menu_page_data->indev_group, ipinfo_label);

    lv_group_set_default(default_group);
}
