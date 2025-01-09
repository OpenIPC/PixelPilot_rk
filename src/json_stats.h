#ifndef JSON_STATS_H
#define JSON_STATS_H

#include <pthread.h>

extern int json_stats_thread_signal;

void *__JSON_STATS_THREAD__(void *param);

#endif
