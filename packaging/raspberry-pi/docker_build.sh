#!/usr/bin/env bash
# Builds laser_daemon for the Raspberry Pi (32-bit ARMv7, armhf). Runs INSIDE
# a debian:trixie linux/arm/v7 container started by build_daemon.sh:
# /src = this repo (read-only), /out = where laser_daemon is written.
# libstdc++/libgcc are linked statically; the remaining shared libraries
# (libc, libusb, libudev) are part of Raspberry Pi OS Lite.
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build git pkg-config libusb-1.0-0-dev ca-certificates >/dev/null
git clone -q --depth=1 https://github.com/sebleedelisle/libera-laser.git /libera
(cd /libera && git submodule update --init --recursive -q)
cp -r /src/backend /work
cmake -S /work -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIBERA_DIR=/libera \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build /build -j"$(nproc)"
cp /build/laser_daemon /out/
strip /out/laser_daemon
ldd /out/laser_daemon
