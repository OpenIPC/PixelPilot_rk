#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "input.h"
#include "menu.h"
#include "gsmenu/helper.h"               /* find_resource_file() */
#include "gsmenu/styles.h"
#include "gsmenu/gs_dvrplayer.h"
#include "gsmenu/air_txprofiles.h"
#include "gsmenu/colmenu.h"
#include "gsmenu/colmenu_pages.h"
#include "gsmenu/gs_connection_checker.h" /* update_network_status() */
#include "lvosd.h"

/* Focused by input.cpp's toggle_screen() to open the menu. Was in the old ui.c. */
lv_group_t * main_group;
extern lv_group_t * osd_group;
extern bool menu_active;
extern uint64_t gtotal_tunnel_data;   /* tunnel bytes from the VTX (drone) */

/* WFB vs APFPV receiver mode. Owned here (not in the old menu's gs_system.c) so
 * it outlives the old menu. */
enum RXMode RXMODE = WFB;

/* DVR backend (defined in main.cpp / dvr.cpp). */
extern int dvr_enabled;
void dvr_start_all(void);
void dvr_stop_all(void);

/* Toggle DVR recording (hardware button, from input.cpp). Formerly poked an
 * lv_switch that only the old menu created — which was NULL under the column
 * menu — so drive the recorder directly instead. */
void toggle_rec_enabled(void)
{
#ifndef USE_SIMULATOR
    if(dvr_enabled) dvr_stop_all();
    else            dvr_start_all();
#else
    dvr_enabled = !dvr_enabled;
    printf("toggle_rec_enabled -> %s\n", dvr_enabled ? "recording" : "stopped");
#endif
    /* keep the menu's "Enabled" switch in sync if the DVR page is open */
    colmenu_reflect_switch("rec_enabled", dvr_enabled != 0);
}

/* Return the current recording state (runtime, not config). Called by colmenu to
 * sync the Display page's Enabled switch on page open. */
int menu_is_recording(void)
{
    return dvr_enabled;
}

/* Drone detection: watch the tunnel-data counter; when it's flowing the drone
 * is present and its pages become active, otherwise they grey out. Mirrors the
 * old ui.c:check_connection_timer, but drives the column menu.
 *
 * Two modes: in WFB the WFB CLI thread fills gtotal_tunnel_data from tunnel
 * traffic; in APFPV nothing does, so we refresh it here from the wlx wifi
 * interface's RX bytes (update_network_status), same as the old timer. */
static void drone_detect_timer(lv_timer_t * t)
{
    (void)t;
    static uint64_t last_value    = 0;
    static uint32_t last_increase = 0;
    static bool     detected      = false;

    if(RXMODE == APFPV) update_network_status();
    uint64_t cur = gtotal_tunnel_data;
    bool now = detected;
    if(cur > last_value)                                    { last_increase = lv_tick_get(); now = true; }
    else if(cur < last_value)                               { now = false; }   /* reset/wrap */
    else if(detected && lv_tick_elaps(last_increase) > 2000){ now = false; }   /* timeout    */
    last_value = cur;

    if(now != detected) {
        detected = now;
        setenv("GSMENU_VTX_DETECTED", detected ? "1" : "0", 1);
        colmenu_set_drone_detected(detected);
    }
}

lv_obj_t * menu;
lv_indev_t * indev_drv;
lv_group_t * default_group;
lv_obj_t * pp_menu_screen;
lv_obj_t * pp_osd_screen;
lv_obj_t * dvr_screen;
lv_obj_t * txprofiles_screen;

/* Back at the root column closes the menu (was ui.c:back_event_handler). */
static void colmenu_return_to_osd(void * ctx)
{
    (void)ctx;
    lv_screen_load(pp_osd_screen);
    lv_indev_set_group(indev_drv, osd_group);
    menu_active = false;
}

/**
 * PP Main Menu — the cascading column menu (colmenu). toggle_screen() in
 * input.cpp loads pp_menu_screen and focuses main_group to open it.
 */

/* The OpenIPC header/logo bar at the top of the menu screen (was ui.c). */
static lv_obj_t * pp_header_create(lv_obj_t * screen) {
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LV_PCT(100), LV_PCT(20));
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0b0e14), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(header, 210, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* thin accent line along the bottom edge for a modern header */
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(header, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(header, lv_color_hex(0x4c60d8), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *obj = lv_img_create(header);
    lv_image_set_src(obj, find_resource_file("OpenIPC__OPENIPC_logo_white.png"));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_image_set_scale(obj, lv_disp_get_ver_res(NULL) / 30);
    return header;
}

void pp_menu_main(void)
{
    style_init();

    indev_drv = create_virtual_keyboard();
    default_group = lv_group_create();
    lv_group_set_default(default_group);
    lv_indev_set_group(indev_drv, default_group);

    pp_menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(pp_menu_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(pp_menu_screen, LV_OBJ_FLAG_SCROLLABLE);
    pp_header_create(pp_menu_screen);

    lv_obj_t * cm_cont = lv_obj_create(pp_menu_screen);
    lv_obj_set_pos(cm_cont, 0, LV_PCT(20));
    lv_obj_set_size(cm_cont, LV_PCT(100), LV_PCT(80));
    lv_obj_set_style_bg_opa(cm_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cm_cont, 0, 0);
    lv_obj_set_style_pad_all(cm_cont, 0, 0);

    colmenu_init(cm_cont);
    colmenu_show(colmenu_pages_root());
    main_group = colmenu_root_group();
    colmenu_set_root_back(colmenu_return_to_osd, NULL);

    /* drone pages start greyed until the VTX is detected */
    setenv("GSMENU_VTX_DETECTED", "0", 1);
    lv_timer_create(drone_detect_timer, 500, NULL);

    /* Separate device screens the menu launches (DVR player, TX profiles). */
    dvr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(dvr_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    dvr_player_screen_init();

    txprofiles_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(txprofiles_screen, LV_OPA_TRANSP, LV_PART_MAIN);
    create_table(txprofiles_screen);

    pp_osd_main();
    lv_screen_load(pp_osd_screen);
}
