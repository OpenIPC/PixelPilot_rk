#pragma once
#include <stddef.h>

/*
 * Background channel-utilization scanner for Realtek rtl88x2eu-family USB WiFi
 * cards, reimplemented from https://github.com/tipoman9/Spectrum/blob/main/8812eu.py
 * in C++ (no Python/pip dependency) for use by the LVGL spectrum screen.
 *
 * The driver exposes per-channel quality/utilization stats under
 * /proc/net/<rtl driver>/<iface>/chan_info, refreshed by an `iw <iface> scan
 * passive`. Frequencies come from `iw dev <iface> info` + `iw phy <phy> info`.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_MAX_IFACES 4
#define SPECTRUM_MAX_ROWS   64

typedef struct {
    int channel;
    int freq_mhz;            /* 0 if unknown */
    int quality_pct;
    int availability_pct;
    int util_pct;
    int wifi_util_pct;
    int interference_pct;
} spectrum_channel_t;

typedef struct {
    char name[16];
    char bandwidth[16];      /* e.g. "20MHz", empty if unknown */
    int  row_count;
    spectrum_channel_t rows[SPECTRUM_MAX_ROWS];
} spectrum_iface_t;

/* Start/stop the background scan thread (idempotent). The scan drives `iw
 * <iface> scan passive`, which takes real air time, so the screen starts it on
 * open and stops it on close instead of scanning continuously in the background. */
void spectrum_scanner_start(void);
void spectrum_scanner_stop(void);

/* Copy the latest snapshot into out[0..max_ifaces). Returns the number of
 * interfaces written. Thread-safe: takes an internal lock and copies, so the
 * caller never holds a pointer the worker thread could mutate concurrently. */
int spectrum_scanner_snapshot(spectrum_iface_t * out, int max_ifaces);

#ifdef __cplusplus
}
#endif
