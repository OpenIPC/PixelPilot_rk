#!/usr/bin/env bash
set -euo pipefail

# Build helper for the target (headless DRM + GBM/EGL/GLES2).
# Usage on target: ./tools/build_on_target.sh

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-gl"
APT_UPDATED=0
SUDO=""

if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

ensure_dep() {
    local pkg="$1"
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "Installing missing package: $pkg"
        if [ "$APT_UPDATED" -eq 0 ]; then
            $SUDO apt-get update || true
            APT_UPDATED=1
        fi
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$pkg"
    fi
}

prep_mali() {
    if [ -e /dev/mali0 ] || [ -e /dev/mali ]; then
        return
    fi
    if ! command -v modprobe >/dev/null 2>&1; then
        echo "[mali] modprobe not available; skipping."
        return
    fi
    if ! lsmod | grep -q '^kbase'; then
        $SUDO modprobe kbase || $SUDO modprobe mali || true
    fi
    if [ -e /dev/mali0 ] && [ ! -e /dev/mali ]; then
        $SUDO ln -sf /dev/mali0 /dev/mali || true
    fi
}

echo "[build] ensuring dependencies..."
ensure_dep cmake
ensure_dep g++
ensure_dep ninja-build || true
ensure_dep libdrm-dev
ensure_dep libgbm-dev
ensure_dep libegl1-mesa-dev
ensure_dep libgles2-mesa-dev
ensure_dep libgstreamer1.0-dev
ensure_dep libgstreamer-plugins-base1.0-dev
ensure_dep libgstreamer-plugins-bad1.0-dev
ensure_dep librockchip-mpp-dev || true
ensure_dep libspdlog-dev
ensure_dep nlohmann-json3-dev
ensure_dep libyaml-cpp-dev
ensure_dep libgpiod-dev

echo "[build] preparing Mali driver (best effort)..."
prep_mali

echo "[build] configuring..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

GENERATOR="-G Ninja"
command -v ninja >/dev/null 2>&1 || GENERATOR=""

cmake ${GENERATOR} -DCMAKE_BUILD_TYPE=Release "${ROOT_DIR}"

echo "[build] building..."
cmake --build . -- -j"$(nproc)"

echo "[build] done. Binary at ${BUILD_DIR}/pixelpilot"
