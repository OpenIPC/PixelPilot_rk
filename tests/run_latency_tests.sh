#!/usr/bin/env bash
# Standalone test runner for the latency_probe unit + integration tests.
# Bypasses the full CMake build (which needs Rockchip MPP/RGA and DRM).
# Requires: g++ (C++17), Catch2 headers, libspdlog, libfmt on the include/link path.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT="$ROOT/build-test"
mkdir -p "$OUT"

# -DUSE_SIMULATOR makes src/osd.h skip its drm.h include, so we can pull
# in osd.h without the full Rockchip toolchain.
g++ -std=c++17 -Wall -Wextra -O0 -g \
    -DUSE_SIMULATOR \
    -I"$ROOT/src" \
    -o "$OUT/latency_probe_tests" \
    "$HERE/standalone_main.cpp" \
    "$HERE/test_latency_probe.cpp" \
    "$HERE/test_latency_probe_integration.cpp" \
    "$HERE/osd_stub.cpp" \
    "$ROOT/src/latency_probe.cpp" \
    -lspdlog -lfmt -lpthread

"$OUT/latency_probe_tests" "$@"
