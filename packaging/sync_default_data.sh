#!/usr/bin/env bash
# Copies the maintainer's current user data (cues, Magic Touch layout,
# lasers, Beam settings) from
# the app data directory into backend/, where the release workflow
# picks them up and ships them as the first-run defaults for new users (see
# resolve_data_file() in backend/laser_daemon.cpp - a missing data file is
# seeded from a same-named file next to the binary / in Resources/).
#
# Also mirrors the ILDA files the cues play (data dir ilda/ -> backend/ilda/,
# only those cues.json refers to - see seed_ilda_files() in laser_daemon.cpp).
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

# ILDA files: exactly the ones the synced cues use.
mkdir -p "$DEST/ilda"
find "$DEST/ilda" -maxdepth 1 -name '*.ild' -delete
if [[ -s "$DEST/cues.json" ]]; then
  python3 -c 'import json,sys
for c in json.load(open(sys.argv[1])).values():
    f = c.get("ilda_file", "")
    if c.get("shape") == "ilda" and f: print(f)' "$DEST/cues.json" | sort -u | while read -r f; do
    if [[ -s "$DATA_DIR/ilda/$f" ]]; then
      cp "$DATA_DIR/ilda/$f" "$DEST/ilda/$f"
      echo "[sync] ilda/$f"
    else
      echo "[sync] WARNING: ilda/$f is used by a cue but missing in $DATA_DIR/ilda" >&2
    fi
  done
fi

git -C "$DEST/.." status --short -- "${FILES[@]/#/backend/}" backend/ilda
