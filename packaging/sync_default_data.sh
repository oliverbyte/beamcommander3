#!/usr/bin/env bash
# Copies the maintainer's current user data (cues, Magic Touch layout) from
# the app data directory into backend/, where the release workflow
# picks them up and ships them as the first-run defaults for new users (see
# resolve_data_file() in backend/laser_daemon.cpp - a missing data file is
# seeded from a same-named file next to the binary / in Resources/).
#
# Also copies this rig's lasers.json and settings.json into
# packaging/raspberry-pi/rig/ - used only by the Raspberry Pi SD card build
# (packaging/raspberry-pi/make_sdcard.sh), never shipped in releases: they
# hold this rig's DAC IP addresses/arm state and projector tuning,
# meaningless (or wrong) on anyone else's network.
#
# Run this before every release (= every push to main), then commit the
# changed files. Not copied on purpose:
# - midi_map.json: the app never writes it - backend/midi_map.json in the repo
#   is the source of truth, the app data copy is just a stale first-run seed.
set -euo pipefail

FILES=(cues.json magic_touch.json)
RIG_FILES=(lasers.json settings.json)

case "$(uname -s)" in
  Darwin) DATA_DIR="$HOME/Library/Application Support/BeamCommander3" ;;
  MINGW*|MSYS*|CYGWIN*) DATA_DIR="$APPDATA/BeamCommander3" ;;
  *) DATA_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/BeamCommander3" ;;
esac
DEST="$(cd "$(dirname "$0")/../backend" && pwd)"
RIG_DEST="$(cd "$(dirname "$0")/raspberry-pi/rig" && pwd)"

copy() { # copy <file> <destination dir>
  local f="$1" dest="$2" src="$DATA_DIR/$1"
  if [[ ! -s "$src" ]]; then
    echo "[sync] skip $f (not found in $DATA_DIR)"
    return
  fi
  if ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$src" 2>/dev/null; then
    echo "[sync] ERROR: $src is not valid JSON - not copied" >&2
    exit 1
  fi
  cp "$src" "$dest/$f"
  echo "[sync] $f <- $src"
}

for f in "${FILES[@]}"; do copy "$f" "$DEST"; done
for f in "${RIG_FILES[@]}"; do copy "$f" "$RIG_DEST"; done

git -C "$DEST/.." status --short -- "${FILES[@]/#/backend/}" "${RIG_FILES[@]/#/packaging/raspberry-pi/rig/}"
