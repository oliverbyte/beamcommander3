#!/usr/bin/env bash
# CFBundleExecutable launcher for the BeamCommander3.app bundle (built by
# .github/workflows/release.yml). Starts the bundled laser_daemon,
# waits for it to come up, then opens the UI in the default browser.
# Quitting the app (Dock icon > Quit, or Cmd-Q) sends SIGTERM here, which
# the trap below forwards to the daemon so it doesn't keep running orphaned.
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"           # .../Contents/MacOS
RES="$(cd "$DIR/../Resources" && pwd)"         # .../Contents/Resources
cd "$RES"                                      # so ./frontend_dist resolves

"$DIR/laser_daemon" >/tmp/beamcommander3.log 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; exit 0' INT TERM EXIT

for _ in $(seq 1 40); do
    curl -s -o /dev/null --max-time 1 http://localhost:8000/api/state && break
    sleep 0.5
done

open "http://localhost:8000"
wait "$PID"
