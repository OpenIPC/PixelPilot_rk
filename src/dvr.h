#ifndef DVR_H
#define DVR_H

#include <string>

// DVR recording is done entirely in-pipeline by the receiver's splitmuxsink
// recorders (raw + re-encode, see gstrtpreceiver.cpp) — the old minimp4 `Dvr`
// muxer has been retired. This header retains only the small shared bits: the
// recording-mode enum, the global record flag, and the filename helper.

enum DvrMode { DVR_MODE_RAW = 0, DVR_MODE_REENCODE = 1, DVR_MODE_BOTH = 2 };

// Master record flag (1 while recording). Read by the encoder/frame pacer and
// mirrored to the receiver's recorders.
extern int dvr_enabled;

// Resolve the next DVR recording's base path from a filename template WITHOUT
// the extension: <dir>/[NNNN_]<strftime(template)> minus a trailing ".mp4".
// Honours --dvr-sequenced-files. Returns "" if the directory does not exist.
std::string dvr_next_base_path(const char* filename_template, bool with_sequence);

#endif
