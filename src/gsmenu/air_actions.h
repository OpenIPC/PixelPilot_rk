#pragma once
#include <stddef.h>
#include "../menu.h"
extern MenuAction airactions[MAX_ACTIONS];
extern size_t airactions_count;

void create_air_actions_menu(lv_obj_t * parent);