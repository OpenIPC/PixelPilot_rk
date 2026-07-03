#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include "../../lvgl/lvgl.h"
#include "../menu.h"       /* enum RXMode, RXMODE */
#include "helper.h"
#include "styles.h"
#include "../WiFiRSSIMonitor.h"

extern lv_indev_t * indev_drv;

static bool file_exists(const char *path) {
    lv_fs_file_t f;
    lv_fs_res_t res = lv_fs_open(&f, path, LV_FS_MODE_RD);
    if(res == LV_FS_RES_OK) {
        lv_fs_close(&f);
        return true;
    }
    return false;
}

/* Resolve a bundled resource (icon/logo) to an LVGL "A:" path, trying the
 * installed locations then the source tree. */
const char* find_resource_file(const char* relative_path) {
    static char path[256];
    const char* prefixes[] = {
        "/usr/local/share/pixelpilot",
        "/usr/share/pixelpilot",
        "./src/icons",
    };
    for(size_t i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "A:%s/%s", prefixes[i], relative_path);
        if(file_exists(path)) {
            return path;
        }
    }
    return NULL;
}

/* Find the first focusable (checkable/clickable, non-disabled, grouped) object
 * in a subtree — used to place focus when a page opens. */
lv_obj_t * find_first_focusable_obj(lv_obj_t * parent) {
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return NULL;
    for (int i = 0; i < lv_obj_get_child_cnt(parent); i++) {
        lv_obj_t * child = lv_obj_get_child(parent, i);

        if ((lv_obj_has_flag(child, LV_OBJ_FLAG_CHECKABLE) || lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) && ! lv_obj_has_state(child, LV_STATE_DISABLED)) {
            lv_group_t * group = lv_obj_get_group(child);
            if (group != NULL) {
                return child;
            }
        }

        lv_obj_t * result = find_first_focusable_obj(child);
        if (result) {
            return result;
        }
    }

    return NULL;
}

/* Apply the environment for the current RX mode (WFB vs APFPV). */
void gsmenu_toggle_rxmode() {
    switch (RXMODE)
    {
    case APFPV:
        setenv("REMOTE_IP" , "192.168.0.1", 1);
        setenv("AIR_FIRMWARE_TYPE" , "apfpv", 1);
        break;
    case WFB:
        setenv("REMOTE_IP" , "10.5.0.10", 1);
        setenv("AIR_FIRMWARE_TYPE" , "wfb", 1);
#ifndef USE_SIMULATOR
        wifi_rssi_monitor_reset();
#endif
        break;

    default:
        break;
    }
}

typedef struct {
    lv_obj_t   *mbox;
    lv_group_t *prev_group;
    lv_group_t *prev_default_group;
    lv_group_t *dialog_group;
} restart_dialog_ctx_t;

static void restart_dialog_btn_cb(lv_event_t *e) {
    restart_dialog_ctx_t *ctx = (restart_dialog_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);

    bool is_yes = (lv_obj_get_index(btn) == 0);

    lv_group_set_default(ctx->prev_default_group);
    lv_indev_set_group(indev_drv, ctx->prev_group);
    lv_group_delete(ctx->dialog_group);
    lv_msgbox_close(ctx->mbox);
    free(ctx);

    if (is_yes)
        raise(SIGHUP);
}

void theme_msgbox(lv_obj_t *mbox) {
    lv_obj_add_style(mbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *header = lv_msgbox_get_header(mbox);
    if (header)
        lv_obj_add_style(header, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    if (footer)
        lv_obj_add_style(footer, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content)
        lv_obj_add_style(content, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* For modal msgboxes (created with a NULL parent) the theme gives the
     * backdrop a bright grey 50% tint. Darken it so it dims the video behind
     * the dialog instead of washing it out. */
    lv_obj_t *backdrop = lv_obj_get_parent(mbox);
    if (backdrop && lv_obj_check_type(backdrop, &lv_msgbox_backdrop_class)) {
        lv_obj_set_style_bg_color(backdrop, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(backdrop, 160, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void show_restart_notice(void) {
    lv_group_t *prev_group         = lv_indev_get_group(indev_drv);
    lv_group_t *prev_default_group = lv_group_get_default();
    lv_group_t *dialog_group       = lv_group_create();
    lv_group_set_default(dialog_group);

    lv_obj_t *top  = lv_layer_top();
    lv_obj_t *mbox = lv_msgbox_create(top);
    lv_obj_t *backdrop = lv_obj_get_child_by_type(top, 0, &lv_msgbox_backdrop_class);
    if (backdrop)
        lv_obj_swap(backdrop, mbox);

    /* give it room so the title/text don't wrap into a squeezed box, and centre it */
    lv_obj_set_width(mbox, 520);
    lv_obj_set_style_pad_all(mbox, 20, LV_PART_MAIN);
    lv_obj_center(mbox);

    lv_msgbox_add_title(mbox, LV_SYMBOL_WARNING " Restart required");
    lv_msgbox_add_text(mbox, "A restart is required to apply the new resolution.\nRestart now?");
    lv_obj_t *yes_btn = lv_msgbox_add_footer_button(mbox, LV_SYMBOL_OK " Yes");
    lv_obj_t *no_btn  = lv_msgbox_add_footer_button(mbox, LV_SYMBOL_CLOSE " No");

    lv_obj_add_style(yes_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(yes_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(no_btn,  &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(no_btn,  &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    theme_msgbox(mbox);

    restart_dialog_ctx_t *ctx = malloc(sizeof(restart_dialog_ctx_t));
    ctx->mbox               = mbox;
    ctx->prev_group         = prev_group;
    ctx->prev_default_group = prev_default_group;
    ctx->dialog_group       = dialog_group;

    lv_indev_set_group(indev_drv, dialog_group);
    /* The key that opened this dialog (e.g. ENTER selecting the resolution) is
     * still pressed; without this its release would immediately "click" the
     * freshly focused Yes button. Wait for a fresh press. */
    lv_indev_wait_release(indev_drv);

    lv_obj_add_event_cb(yes_btn, restart_dialog_btn_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(no_btn,  restart_dialog_btn_cb, LV_EVENT_CLICKED, ctx);

    /* default to the safe choice */
    lv_group_focus_obj(no_btn);
}
