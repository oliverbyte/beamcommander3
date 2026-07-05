#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DAEMON_BIN="$ROOT/backend/laser_daemon"
DAEMON_LOG="${TMPDIR:-/tmp}/bc3-daemon.log"
FRONTEND_LOG="${TMPDIR:-/tmp}/bc3-frontend.log"

# Kill child processes on exit/Ctrl-C — run only once
_cleaned=0
cleanup() {
    [[ $_cleaned -eq 1 ]] && return; _cleaned=1
    echo ""
    echo "[start.sh] Stopping..."
    trap '' INT TERM  # ignore further signals during cleanup
    kill "$DAEMON_PID" "$FRONTEND_PID" 2>/dev/null || true
    wait "$DAEMON_PID" "$FRONTEND_PID" 2>/dev/null || true
    echo "[start.sh] Done."
}
trap cleanup EXIT INT TERM

# --- Build daemon if missing or source is newer ---
if [[ ! -x "$DAEMON_BIN" ]] || [[ "$ROOT/backend/laser_daemon.cpp" -nt "$DAEMON_BIN" ]]; then
    echo "[start.sh] Building backend..."
    (cd "$ROOT/backend" && bash build.sh)
fi

# --- Start laser_daemon (HTTP :8000 + WS) ---
# Must run with cwd=backend/ - it loads midi_map.json and cues.json via
# relative paths, so launching it from anywhere else silently breaks both
# (MIDI falls back to "no mapping file... MIDI control disabled", and cues
# read/write to a stray cues.json in the wrong directory instead of the
# real one).
(cd "$ROOT/backend" && exec "$DAEMON_BIN" >"$DAEMON_LOG" 2>&1) &
DAEMON_PID=$!
echo "[start.sh] laser_daemon started (pid $DAEMON_PID) — log: $DAEMON_LOG"

# --- Frontend ---
if [[ ! -d "$ROOT/frontend/node_modules" ]]; then
    echo "[start.sh] Installing frontend dependencies..."
    npm --prefix "$ROOT/frontend" install --silent
fi

npm --prefix "$ROOT/frontend" run dev -- --port 5173 <&- >"$FRONTEND_LOG" 2>&1 &
FRONTEND_PID=$!
echo "[start.sh] Frontend started (pid $FRONTEND_PID) — log: $FRONTEND_LOG"

# Wait until both are ready
for i in $(seq 1 10); do
    sleep 1
    BE_OK=0; FE_OK=0
    curl -s --max-time 1 -o /dev/null http://localhost:8000/api/state && BE_OK=1 || true
    curl -s --max-time 1 -o /dev/null http://localhost:5173/          && FE_OK=1 || true
    [[ $BE_OK -eq 1 && $FE_OK -eq 1 ]] && break
done

echo ""
echo "  BeamCommander3 is running"
echo "  → http://localhost:5173"
echo "  → API: http://localhost:8000/api/state"
echo ""
echo "  Press Ctrl-C to stop."
echo ""

wait
