#pragma once
#include <stddef.h>
#include "../menu.h"

extern MenuAction gsactions[MAX_ACTIONS];
extern size_t gsactions_count;

void create_gs_actions_menu(lv_obj_t * parent);