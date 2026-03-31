#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include "lvgl/lvgl.h"
#include "menu.h"
#include "input.h"
#include "gsmenu/helper.h"
#include "gsmenu/air_actions.h"
#include "gsmenu/gs_actions.h"


int dvr_enabled = 0;
uint64_t gtotal_tunnel_data = 0;
bool disable_vsync = false;
const char *dvr_template = "/tmp/record_%Y-%m-%d_%H-%M-%S.mp4";

// Stubs for symbols defined in main.cpp / dvr.cpp
bool enable_live_colortrans = false;
float live_colortrans_gain = 2.5f;
float live_colortrans_offset = -0.15f;

MenuAction airactions[MAX_ACTIONS];
size_t airactions_count = 0;
MenuAction gsactions[MAX_ACTIONS];
size_t gsactions_count = 0;

int dvr_get_mode(void)          { return 0; }
int dvr_reenc_get_osd(void)     { return 0; }
int dvr_reenc_get_fps(void)     { return 30; }
int dvr_reenc_get_bitrate(void) { return 8000; }
int dvr_reenc_get_codec(void)   { return 0; }
int dvr_reenc_get_resolution(void) { return 1; }
int dvr_get_max_size(void)      { return 4000; }
void my_log_cb(lv_log_level_t level, const char * buf)
{
  printf("%s",buf);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_disp_t * disp = lv_sdl_window_create(1920,1080);

    // lv_log_register_print_cb(my_log_cb);

    lv_obj_t * bottom = lv_display_get_layer_bottom(disp);
    lv_obj_t *obj = lv_img_create(bottom);
    lv_image_set_src(obj, find_resource_file("osd-bg-2.png"));
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);

    pp_menu_main();
    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }

}