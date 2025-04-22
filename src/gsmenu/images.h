#pragma once

#ifdef USE_SIMULATOR
#define IMAGE_COUNT 2
extern const lv_img_dsc_t background;
#else
#define IMAGE_COUNT 1
#endif

extern const lv_img_dsc_t img_open_ipc_logo;

typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;

extern const ext_img_desc_t images[IMAGE_COUNT];