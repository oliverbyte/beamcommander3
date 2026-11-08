#!/usr/bin/env bash
# Builds and runs the backend unit tests (tests/test_*.cpp: hardware output
# safety pipeline - safety masks, outlines, blackout/arm gating, zone - and
# show parameter smoothing, flicker). Called by .githooks/pre-push before
# every push - a failing test aborts the push. Can also be run manually.
# Needs only libera-laser's headers (LaserPoint is header-only), not the
# compiled library. Built with UndefinedBehaviorSanitizer by default, so
# undefined behaviour in the pipeline fails the run too. Override with
# TEST_SANITIZERS (e.g. "address,undefined" for ASan as well - not the
# default because ASan hangs even on an empty program with some macOS/
# toolchain combinations; "" disables sanitizers).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BACKEND="$(dirname "$HERE")"
LIBERA_DIR="${LIBERA_DIR:-/tmp/libera-laser}"
CXX="${CXX:-clang++}"
OUT="${TMPDIR:-/tmp}/beamcommander_backend_tests"
SANITIZERS="${TEST_SANITIZERS-undefined}"
SAN_FLAGS=()
if [[ -n "$SANITIZERS" ]]; then
  SAN_FLAGS=(-fsanitize="$SANITIZERS" -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi

# Every tests/test_*.cpp is its own self-contained test program.
for src in "$HERE"/test_*.cpp; do
  bin="${OUT}_$(basename "$src" .cpp)"
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    ${SAN_FLAGS[@]+"${SAN_FLAGS[@]}"} \
    -I"$BACKEND" -I"${LIBERA_DIR}/include" \
    "$src" -o "$bin"
  "$bin"
done
