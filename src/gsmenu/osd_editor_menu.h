#pragma once
/*
 * System Settings → OSD: edit the OSD layout (the --osd-config JSON) live —
 * add, change and delete widgets, pick their facts from lists and move them
 * around with the arrow keys; Save writes the file back.
 */
#include "colmenu.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const colmenu_page_t osd_editor_page;

#ifdef __cplusplus
}
#endif
