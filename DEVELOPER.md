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

## Architecture

```
laser_daemon.cpp
  ├── laser_thread()      single background thread:
  │     - advances an animation clock (G_time)
  │     - generates a Frame from the current LaserState every tick
  │     - always broadcasts the frame over WebSocket at ~30fps (preview),
  │       regardless of hardware connection state
  │     - if a controller is connected + armed + ready, also sends the
  │       same frame to it via libera's core::LaserController API
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
