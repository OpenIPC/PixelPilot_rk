#ifndef WFBCLIPP_H
#define WFBCLIPP_H

extern int wfb_thread_signal;

typedef struct {
	int port;
} wfb_thread_params;

void *__WFB_CLI_THREAD__(void *param);

#endif
