#!/bin/sh
# Build the firmware. Needs PICO_SDK_PATH (or edit the default below).
set -e
cd "$(dirname "$0")"
: "${PICO_SDK_PATH:=$HOME/pico-sdk}"
export PICO_SDK_PATH
: "${BUILD_DIR:=build}"
cmake -S . -B "$BUILD_DIR" -G Ninja "$@"
cmake --build "$BUILD_DIR"
echo "Built: $BUILD_DIR/pico_wii_ethernet.uf2"
