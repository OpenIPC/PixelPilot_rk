#ifndef OSD_H
#define OSD_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

extern int enable_osd;
extern bool osd_custom_message;
extern pthread_mutex_t osd_mutex;

#define TAG_MAX_LEN 64

typedef struct {
    char key[TAG_MAX_LEN];
    char val[TAG_MAX_LEN];
} osd_tag;

#ifdef __cplusplus
extern "C" {
#endif
// Batch functions are when you publish several facts from the same place
// It has optimized publishing algorithm - takes the lock only once per-batch
void *osd_batch_init(uint n);
void osd_publish_batch(void *batch);
void osd_add_bool_fact(void *batch, char const *name, osd_tag *tags, int n_tags, bool value);
void osd_add_int_fact(void *batch, char const *name, osd_tag *tags, int n_tags, long value);
void osd_add_uint_fact(void *batch, char const *name, osd_tag *tags, int n_tags, ulong value);
void osd_add_double_fact(void *batch, char const *name, osd_tag *tags, int n_tags, double value);
void osd_add_str_fact(void *batch, char const *name, osd_tag *tags, int n_tags, const char *value);

// Publish individual facts
void osd_publish_bool_fact(char const *name, osd_tag *tags, int n_tags, bool value);
void osd_publish_int_fact(char const *name, osd_tag *tags, int n_tags, long value);
void osd_publish_uint_fact(char const *name, osd_tag *tags, int n_tags, ulong value);
void osd_publish_double_fact(char const *name, osd_tag *tags, int n_tags, double value);
void osd_publish_str_fact(char const *name, osd_tag *tags, int n_tags, const char *value);
#ifdef __cplusplus
}
#endif

#endif
