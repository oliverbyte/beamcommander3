#!/usr/bin/env bash
set -euo pipefail

LIBERA_DIR="${LIBERA_DIR:-/tmp/libera-laser}"
LIBUSB_FLAGS="$(pkg-config --cflags --libs libusb-1.0 2>/dev/null || echo "-L/usr/local/Cellar/libusb/1.0.30/lib -lusb-1.0")"

clang++ -std=c++17 -O2 \
  -I"$(dirname "$0")" \
  -I"${LIBERA_DIR}/include" \
  -I"${LIBERA_DIR}/libs/asio/include" \
  -I"${LIBERA_DIR}/libs/helios_dac/sdk/cpp" \
  laser_daemon.cpp \
  "${LIBERA_DIR}/build/release/liblibera-core.a" \
  ${LIBUSB_FLAGS} \
  -framework CoreFoundation -framework IOKit \
  -framework CoreAudio -framework AudioToolbox \
  -lpthread \
  -o laser_daemon

echo "Built: $(pwd)/laser_daemon"
