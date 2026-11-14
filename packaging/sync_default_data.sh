#!/usr/bin/env bash
# Copies the maintainer's current user data (cues, Magic Touch layout,
# lasers, Beam settings) from
# the app data directory into backend/, where the release workflow
# picks them up and ships them as the first-run defaults for new users (see
# resolve_data_file() in backend/laser_daemon.cpp - a missing data file is
# seeded from a same-named file next to the binary / in Resources/).
#
# Run this before every release (= every push to main), then commit the
# changed files. Not copied on purpose:
# - midi_map.json: the app never writes it - backend/midi_map.json in the repo
#   is the source of truth, the app data copy is just a stale first-run seed.
set -euo pipefail

FILES=(cues.json magic_touch.json lasers.json settings.json)

case "$(uname -s)" in
  Darwin) DATA_DIR="$HOME/Library/Application Support/BeamCommander3" ;;
  MINGW*|MSYS*|CYGWIN*) DATA_DIR="$APPDATA/BeamCommander3" ;;
  *) DATA_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/BeamCommander3" ;;
esac
DEST="$(cd "$(dirname "$0")/../backend" && pwd)"

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

git -C "$DEST/.." status --short -- "${FILES[@]/#/backend/}"
