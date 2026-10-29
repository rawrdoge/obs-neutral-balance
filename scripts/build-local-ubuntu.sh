#!/usr/bin/env bash
# build-local-ubuntu.sh - build obs-neutral-balance locally on Ubuntu/Debian.
# Mirrors the linux job in .github/workflows/build.yaml, so results here
# predict CI results. Requires: sudo, ~10 GB disk, ~15-20 min first run.
set -euo pipefail

OBS_TAG="31.1.1"

echo "==> Installing build dependencies (same list as CI)"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential git cmake ninja-build pkg-config extra-cmake-modules \
  libx11-dev libxcb1-dev libxcb-randr0-dev libxcb-shm0-dev \
  libxcb-xfixes0-dev libxcb-xinerama0-dev \
  libxcomposite-dev libxdamage-dev libxrandr-dev libxfixes-dev \
  libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libudev-dev qt6-base-dev uthash-dev libjansson-dev \
  uuid-dev libglvnd-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxshmfence-dev \
  libx11-xcb-dev libgbm-dev libdrm-dev \
  libpulse-dev libxcb-xinput-dev libv4l-dev libspeexdsp-dev \
  libavfilter-dev libavdevice-dev libpostproc-dev \
  libva-dev libva-drm2 libva-glx2 libva-x11-2 libpci-dev

if [ ! -d obs-studio ]; then
  echo "==> Cloning obs-studio $OBS_TAG"
  git clone --depth 1 --branch "$OBS_TAG" https://github.com/obsproject/obs-studio obs-studio
  git -C obs-studio submodule update --init --recursive --depth 1
fi

echo "==> Configuring obs-studio (libobs only)"
cmake -S obs-studio -B obs-studio/build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_UI=OFF -DENABLE_SCRIPTING=OFF -DBUILD_TESTS=OFF \
  -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF -DENABLE_AJA=OFF \
  -DENABLE_NEW_MPEGTS_OUTPUT=OFF

echo "==> Building libobs"
cmake --build obs-studio/build --target libobs -j"$(nproc)"

echo "==> Installing libobs component"
cmake --install obs-studio/build --prefix "$PWD/libobs-prefix" --component libobs || true

echo "==> Ensuring CMake package config (same discovery logic as CI)"
if ! find libobs-prefix -name libobsConfig.cmake | grep -q .; then
  hdr=$(find libobs-prefix -name obs-module.h | head -1)
  [ -z "$hdr" ] && hdr="obs-studio/libobs"
  inc_dir=$(dirname "$hdr")
  inc_list="$inc_dir"
  [ -d obs-studio/build/libobs ] && inc_list="$inc_list;obs-studio/build/libobs"
  gen=$(find obs-studio/build -name obsconfig.h | head -1)
  [ -n "$gen" ] && inc_list="$inc_list;$(dirname "$gen")"
  lib=$(find libobs-prefix obs-studio/build -name 'libobs.so' | head -1)
  mkdir -p libobs-prefix/lib/cmake/libobs
  cat > libobs-prefix/lib/cmake/libobs/libobsConfig.cmake <<EOF
if(NOT TARGET OBS::libobs)
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "$lib"
    INTERFACE_INCLUDE_DIRECTORIES "$inc_list")
endif()
set(libobs_FOUND TRUE)
EOF
  echo "   synthesized config: includes=$inc_list lib=$lib"
fi

echo "==> Building plugin"
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/libobs-prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

echo "==> Done. Module: build/obs-neutral-balance.so"
echo "    Install with: cp build/obs-neutral-balance.so ~/.config/obs-studio/plugins/obs-neutral-balance/obs-plugins/"
