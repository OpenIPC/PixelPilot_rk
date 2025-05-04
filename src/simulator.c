#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include "lvgl/lvgl.h"
#include "menu.h"
#include "input.h"
#include "gsmenu/images.h"


int dvr_enabled = 0;
uint64_t gtotal_tunnel_data = 0;
bool disable_vsync = false;

void my_log_cb(lv_log_level_t level, const char * buf)
{
  printf("%s\n",buf);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_disp_t * disp = lv_sdl_window_create(1920,1080);

    // lv_log_register_print_cb(my_log_cb);

    lv_obj_t * bottom = lv_display_get_layer_bottom(disp);
    lv_obj_t *obj = lv_img_create(bottom);
    lv_img_set_src(obj, &background);

    pp_menu_main();
    while (1) {
        handle_keyboard_input();
        lv_task_handler();
        usleep(5000);
    }

}