#!/usr/bin/env bash
set -euo pipefail

LIBERA_DIR="${LIBERA_DIR:-/tmp/libera-laser}"
LIBERA_LIB="${LIBERA_DIR}/build/release/liblibera-core.a"
LIBUSB_FLAGS="$(pkg-config --cflags --libs libusb-1.0 2>/dev/null || echo "-L/usr/local/Cellar/libusb/1.0.30/lib -lusb-1.0")"

# libera-laser lives under /tmp, which is not persistent (cleared on
# reboot/by the OS). Self-heal by cloning + building it fresh if it's
# missing or was left in a corrupted/incomplete state.
if [[ ! -f "$LIBERA_LIB" ]]; then
    echo "[build.sh] $LIBERA_LIB not found — (re)building libera-laser..."
    rm -rf "$LIBERA_DIR"
    git clone --depth=1 https://github.com/sebleedelisle/libera-laser.git "$LIBERA_DIR"
    (cd "$LIBERA_DIR" && git submodule update --init --recursive)
    (cd "$LIBERA_DIR" && cmake --preset release \
        -DLIBERA_BUILD_EXAMPLES=OFF \
        -DLIBERA_BUILD_TESTS=OFF \
        -DLIBERA_BUILD_LASER_TOOL=OFF)
    (cd "$LIBERA_DIR" && cmake --build --preset release)
fi

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
  -framework CoreMIDI \
  -lpthread \
  -o laser_daemon

echo "Built: $(pwd)/laser_daemon"
