// No-op OSD pubsub implementations for the standalone latency_probe tests.
// The tests override the publish path via set_publish_overrides_for_test(),
// so these symbols only need to satisfy the linker.
#include "osd.h"

extern "C" {
void osd_publish_uint_fact(char const*, osd_tag*, int, ulong)  {}
void osd_publish_int_fact (char const*, osd_tag*, int, long)   {}
}
