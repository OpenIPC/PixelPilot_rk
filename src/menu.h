#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#define MAX_ACTIONS 5
#define MAX_LABEL_LEN 50
#define MAX_ACTION_LEN 500

typedef struct {
    char label[MAX_LABEL_LEN];
    char action[MAX_ACTION_LEN];
} MenuAction;

void pp_menu_main(void);