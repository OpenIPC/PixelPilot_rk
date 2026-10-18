#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "menu.h"
#include "input.h"
#include "gsmenu/helper.h"
#include "osd.h"

void sim_facts_start(void);   /* sim_facts.c */


int dvr_enabled = 0;
uint32_t refresh_frequency_ms = 100;   /* OSD widget refresh (main.cpp's --osd-refresh) */
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

// Simulator stubs for restream API (real impl lives in gstrtpreceiver.cpp,
// which is not part of the simulator build). Stateful so the menu rows can
// actually be exercised; the client list is faked.
static bool sim_restream_enabled = false;
static char sim_restream_ip[64] = "";
bool restream_get_enabled() { return sim_restream_enabled; }
void restream_set_enabled(bool enabled) { sim_restream_enabled = enabled; }
void restream_scan_clients(char* buf, size_t buf_len) {
    if (buf && buf_len) snprintf(buf, buf_len, "Auto\n192.168.1.23\n192.168.1.47");
}
const char* restream_get_manual_ip() { return sim_restream_ip; }
void restream_set_manual_ip(const char* ip) {
    if (ip && ip[0] && strcmp(ip, "Auto") != 0) snprintf(sim_restream_ip, sizeof(sim_restream_ip), "%s", ip);
    else sim_restream_ip[0] = '\0';
}

int  audio_get_enabled(void) { return 0; }
void audio_set_enabled(int enabled) { (void)enabled; }
void audio_set_device(const char* device) { (void)device; }
void audio_set_volume(int percent) { (void)percent; }

int main(int argc, char **argv)
{
    /* The menu shells out to `gsmenu.sh` by bare name via popen/system, so it
     * must be on PATH. In the sim the script lives in the working directory
     * (the repo root), and sudo's secure_path drops any PATH sim.sh exports —
     * so prepend the CWD to PATH here, inside the process, where popen/system
     * children inherit it. */
    {
        char cwd[1024];
        const char * old = getenv("PATH");
        if (getcwd(cwd, sizeof(cwd))) {
            char newpath[2048];
            snprintf(newpath, sizeof(newpath), "%s:%s", cwd, old ? old : "");
            setenv("PATH", newpath, 1);
        }
    }

    /* The real OSD engine runs in the sim: --osd-config picks the layout (it is
     * also what the OSD editor saves to), --assets the icon dir (the layout's
     * assets_dir points at the device install path), --res the screen size. */
    const char * osd_config = "./config_osd.json";
    char assets[1024] = "./src/icons";
    int width = 1920, height = 1080;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--osd-config") && i + 1 < argc) osd_config = argv[++i];
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) snprintf(assets, sizeof(assets), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else {
            fprintf(stderr, "usage: %s [--osd-config <file>] [--assets <dir>] [--res WxH]\n", argv[0]);
            return 1;
        }
    }
    char * assets_abs = realpath(assets, NULL);   /* "A:" paths are opened relative to CWD otherwise */

    lv_init();
    lv_disp_t * disp = lv_sdl_window_create(width, height);

    // lv_log_register_print_cb(my_log_cb);

    lv_obj_t * bottom = lv_display_get_layer_bottom(disp);
    lv_obj_t *obj = lv_img_create(bottom);
    lv_image_set_src(obj, find_resource_file("osd-bg-2.png"));
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);

    enable_osd = 1;
    osd_lock();
    osd_engine_start(bottom, width, height, osd_config, assets_abs ? assets_abs : assets);
    pp_menu_main();
    osd_unlock();
    sim_facts_start();

    /* Same locking as the device's OSD thread: everything touching LVGL (keys can
     * too, e.g. 'r' or 't') runs under the OSD lock the fact processor takes. */
    while (1) {
        osd_lock();
        handle_keyboard_input();
        osd_tick_if_due();
        lv_task_handler();
        osd_unlock();
        usleep(5000);
    }

}