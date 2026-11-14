#!/usr/bin/env bash
# Cross-builds laser_daemon for the Raspberry Pi (armhf) in Docker (emulated
# linux/arm/v7, ~10 min on Apple Silicon). Output: $BC3_PI_WORK/out/laser_daemon
# (default work dir ~/bc3-pi, outside the repo).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${BC3_PI_WORK:-$HOME/bc3-pi}"
mkdir -p "$WORK/out"
echo "[pi] building laser_daemon for armhf (log: $WORK/out/build.log)..."
docker run --rm --platform linux/arm/v7 \
  -v "$REPO:/src:ro" -v "$WORK/out:/out" -v "$HERE:/scripts:ro" \
  debian:trixie bash /scripts/docker_build.sh > "$WORK/out/build.log" 2>&1 \
  || { tail -30 "$WORK/out/build.log"; exit 1; }
echo "[pi] built $WORK/out/laser_daemon"
