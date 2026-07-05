# Developer Guide

## Project layout

```
backend/
  laser_daemon.cpp   Single C++ backend: libera-laser output + HTTP + WebSocket
  httplib.h          Vendored header-only HTTP/WebSocket server (yhirose/cpp-httplib)
  build.sh           Compiles laser_daemon (and legacy circle_daemon)
frontend/
  src/components/ControlPanel.vue   All UI controls (shape, color, transform, FX...)
  src/components/LaserScene.vue     Three.js 3D preview, renders WS point stream
  src/composables/useLaserSocket.js REST + WebSocket client, shared reactive state
start.sh             Builds backend if needed, runs both processes, cleans up on Ctrl-C
```

There is no Python anywhere in this project. `backend/laser_daemon` is the
only server process; the frontend is a static Vite dev server that talks to
it directly over HTTP/WebSocket (proxied in dev via `vite.config.js`).

## Prerequisites (macOS)

| Tool | Install |
|------|---------|
| Xcode Command Line Tools | `xcode-select --install` |
| CMake ≥ 3.15 | `brew install cmake` |
| Ninja | `brew install ninja` |
| libusb (for Helios/LaserCube USB) | `brew install libusb` |
| Node.js | `brew install node` |

## 1 — Build libera-laser (one-time)

```sh
git clone --depth=1 https://github.com/sebleedelisle/libera-laser.git /tmp/libera-laser
cd /tmp/libera-laser
git submodule update --init --recursive

cmake --preset release \
  -DLIBERA_BUILD_EXAMPLES=OFF \
  -DLIBERA_BUILD_TESTS=OFF \
  -DLIBERA_BUILD_LASER_TOOL=OFF

cmake --build --preset release
```

Output: `/tmp/libera-laser/build/release/liblibera-core.a`

## 2 — Build laser_daemon

```sh
cd backend
bash build.sh
```

`build.sh` auto-detects the libusb path via `pkg-config` and defaults
`LIBERA_DIR` to `/tmp/libera-laser`. Override if needed:

```sh
LIBERA_DIR=/path/to/libera-laser bash build.sh
```

Raw compiler command (for reference):

```sh
clang++ -std=c++17 -O2 \
  -I. \
  -I${LIBERA_DIR}/include \
  -I${LIBERA_DIR}/libs/asio/include \
  -I${LIBERA_DIR}/libs/helios_dac/sdk/cpp \
  laser_daemon.cpp \
  ${LIBERA_DIR}/build/release/liblibera-core.a \
  $(pkg-config --cflags --libs libusb-1.0) \
  -framework CoreFoundation -framework IOKit \
  -framework CoreAudio -framework AudioToolbox \
  -lpthread \
  -o laser_daemon
```

## 3 — Run everything

```sh
./start.sh
```

Or run pieces manually while developing:

```sh
# Backend only
cd backend && ./laser_daemon --port 8000

# Frontend only (separate terminal)
cd frontend && npm install && npm run dev -- --port 5173 < /dev/null
```

> `< /dev/null` on the Vite command prevents it from suspending waiting for
> keyboard input when run detached/backgrounded.

## Standalone / packaged builds

`laser_daemon` can serve the built frontend itself, so a packaged release
doesn't need Node.js or `npm run dev` at all - just the compiled binary plus
a static asset folder:

```sh
cd frontend && npm ci && npm run build   # produces frontend/dist
cp -r frontend/dist backend/frontend_dist
cd backend && ./laser_daemon             # now serves the UI at :8000 too
```

`laser_daemon` checks for a `./frontend_dist` directory (relative to the
current working directory, override with the `FRONTEND_DIST` env var) at
startup and mounts it at `/` via cpp-httplib's static file server if found -
otherwise it's a no-op (normal dev-mode usage, where Vite's dev server
handles the frontend instead, is unaffected). This is what the macOS release
GitHub Action packages up - see `.github/workflows/release-macos.yml`.

## Architecture

```
laser_daemon.cpp
  ├── laser_thread()      single background thread:
  │     - advances an animation clock (G_time) plus per-parameter phase
  │       accumulators (G_move_phase/G_rotation_phase/G_wave_phase/
  │       G_rainbow_phase) and a smoothed position (G_pos_x_smooth/
  │       G_pos_y_smooth) - see "Animation phase continuity" below
  │     - generates a Frame from the current LaserState every tick
  │     - always broadcasts the frame over WebSocket at ~30fps (preview),
  │       regardless of hardware connection state
  │     - if a controller is connected + armed + ready, also sends the
  │       same frame to it via libera's core::LaserController API
  ├── midi_thread()      optional, macOS/CoreMIDI only: auto-connects to
  │     every MIDI source, dispatches note/CC messages into the same
  │     LaserState - see "MIDI subsystem" below
  └── httplib::Server     REST handlers mutate a global LaserState (G)
                          under G_mtx; GET /api/state serializes it
```

Key design points:

- **Preview independent of hardware.** The old design only generated/broadcast
  frames when a real controller was connected and `isReadyForNewFrame()`. That
  meant the browser preview was frozen with no laser attached. `laser_thread`
  now always generates + broadcasts a frame every ~33ms and *additionally*
  sends it to hardware when available — the two are no longer coupled.
- **Direct-connect, no UDP discovery.** `EtherDreamControllerInfo` is
  constructed directly from the IP the user provides (port 7765, standard
  4095-point buffer) instead of relying on libera's UDP broadcast discovery.
  This avoids `UdpSocket bind_any failed` errors when the discovery port is
  already in use, and connects instantly instead of waiting out a scan.
- **Connect retry backoff.** Hardware connect attempts back off 1s after a
  failure (`next_connect_attempt`) so a persistently-unreachable IP doesn't
  spam TCP connect attempts on every loop iteration.
- **Single non-recursive mutex (`G_mtx`).** Every REST handler that mutates
  `LaserState` must fully release the lock *before* calling `state_to_json()`,
  which locks the same mutex again. Getting this wrong deadlocks the handler's
  worker thread permanently — cpp-httplib's thread pool is small, so a few of
  these in a row makes the whole server unresponsive (looks like a crash, but
  the process is actually just stuck). Always scope `lock_guard` in its own
  `{ }` block or inside a `try { }` that ends before the `res.set_content(...)`
  call.
- **`POST /api/reset`** restores `LaserState` to its default-constructed
  values but preserves `target_ip` so resetting the show doesn't drop the
  hardware connection.
- **No recursive shape/frame restart on param change.** All shape/movement/
  color/etc. parameters are read fresh from `G` every frame — there is no
  "restart the daemon" step anywhere, so slider drags are glitch-free.

### Animation phase continuity

`rotation_speed`, `move_speed`, `wave_speed`, and `rainbow_speed` are never
used directly as `phase = speed * G_time`. That formula looks fine until a
speed *changes*: since `G_time` has already accumulated a large elapsed
value, multiplying it by a new speed lands on a completely different point
in the cycle - a visible jump/reset the instant a slider or knob moves.

Instead, `laser_thread()`'s main loop integrates each parameter's own phase
accumulator (`G_move_phase`, `G_rotation_phase`, `G_wave_phase`,
`G_rainbow_phase`) by `speed * dt` every tick, using whatever speed is
current *right now*. Changing a speed only changes the rate of change from
that point forward; the current position in the cycle is preserved and the
animation stays smooth. These globals are never locked - only
`laser_thread()` ever touches them, matching the existing `G_time` pattern.

`rotation_speed`'s sign is negated at the single point where its phase is
integrated (not in every setter) so the rotation direction is reversed
uniformly regardless of which API set it.

### Position smoothing

`pos_x`/`pos_y` in `LaserState` are a *target*, reported as-is by
`GET /api/state` (so the UI slider never lags). The value actually used
when rendering (`G_pos_x_smooth`/`G_pos_y_smooth`) exponentially slews
toward that target every tick via `smooth_toward()`, a direct port of the
original BeamCommander's `ofApp.cpp` `smoothOne` lambda: tiny moves
(<0.02 normalized) blend extra slowly (hides a MIDI knob's 128-step/7-bit
resolution as visible stepping), while big jumps (>0.25 normalized) get a
boosted alpha so large moves still feel responsive. `make_frame()` reads
the smoothed globals directly, not the raw target fields.

### Cue system

Cues are snapshots of `LaserState` (minus `target_ip`) in
`std::map<int, LaserState> G_cues`, guarded by its own `G_cues_mtx` and
persisted to `backend/cues.json`. **`G_mtx` and `G_cues_mtx` are never held
at the same time** - `do_cue_save/recall/clear()` in laser_daemon.cpp are
the *only* functions allowed to touch `G_cues`, and each one locks at most
one of the two mutexes at a time (lock, copy/assign, unlock - never nested).
Both the `/api/cue/*` HTTP routes and the MIDI dispatcher's `cue:<n>` /
`cue_momentary:<n>` actions call these same helpers instead of duplicating
the logic, so there's exactly one place that has to get the lock order
right. If you add a new cue-related code path, route it through these
helpers rather than touching `G_cues` directly.

### MIDI subsystem (optional, macOS/CoreMIDI)

`midi_thread()` runs a `CFRunLoopRun()` forever on its own thread (required
for CoreMIDI to deliver its read-proc callback), auto-connecting to every
available MIDI source at startup and rescanning every 3s for hot-plugged
devices. Bindings are loaded from `backend/midi_map.json` into
`G_midi_bindings` (guarded by `G_midi_mtx`) rather than hard-coded, since
exact note/CC numbers depend on the controller's firmware mode; unmapped
messages are logged so a real device can be calibrated without recompiling.

- `midi_cc_value()` implements the original BeamCommander mapper's response
  curve (centered/gamma/deadzone/invert/scale/offset) exactly, given a raw
  0-127 int - see README's "CC response curve" table for the field meanings.
- Momentary button actions (`flash`, `blackout_hold`, `motion_hold`,
  `cue_momentary:<n>`) all follow the same pattern: a `do_*_press()` /
  `do_*_release()` pair that snapshots whatever it's about to override and
  restores it on release. These are also called directly from REST routes
  (e.g. `POST /flash/<0|1>`) so a momentary action behaves identically
  whether triggered from the UI, REST, or MIDI - never implement the same
  behavior twice.
- A footswitch is just another input: some pedals send a note, others send
  a CC (commonly CC64, the standard MIDI "sustain pedal" number). CC-based
  momentary actions are handled by treating the curve-mapped value above/
  below 0.5 as press/release inside `midi_apply_cc_action()` - this works
  because `do_flash_press/release()` are idempotent, so calling them
  repeatedly while a pedal holds steady at a value is harmless.
## Debugging a hung backend

If the UI stops responding to changes but the process is still running
(`pgrep -fl laser_daemon` shows it alive), it's almost certainly a mutex
deadlock in a new REST handler — check for a `lock_guard` still in scope when
`state_to_json()` is called. Reproduce with:

```sh
for i in $(seq 1 100); do
  curl -s -X POST http://localhost:8000/api/state \
    -H "Content-Type: application/json" -d '{"intensity":0.5}' >/dev/null &
done
wait
curl -s --max-time 3 http://localhost:8000/api/state && echo OK || echo HUNG
```

## After every major change

- Update `README.md` — endpoint table, quick start, user-facing behavior.
- Update this file — architecture notes, build steps, gotchas.
