#!/bin/bash

# We have to capture the output and explicitly check it for "FAILED" because
# address sanitizer makes executable end with non-zero return code under QEMU.
# So we can't rely solely on return codes
ROOTDIR=/usr/src/PixelPilot_rk

cd $ROOTDIR

OUT=$(LD_LIBRARY_PATH=/usr/local/lib/ ./build/pixelpilot_tests $@ 2>&1)

echo "$OUT"

if [[ $OUT == *"FAILED"* ]]; then
    exit 1
fi
