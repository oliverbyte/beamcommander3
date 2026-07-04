# Developer Guide

## Prerequisites (macOS)

| Tool | Install |
|------|---------|
| Xcode Command Line Tools | `xcode-select --install` |
| CMake ≥ 3.15 | `brew install cmake` |
| Ninja | `brew install ninja` |
| libusb (for Helios/LaserCube USB) | `brew install libusb` |

## 1 — Clone and build libera-laser

```sh
git clone --depth=1 https://github.com/sebleedelisle/libera-laser.git /tmp/libera-laser
cd /tmp/libera-laser
git submodule update --init --recursive

cmake --preset release \
  -DLIBERA_BUILD_EXAMPLES=ON \
  -DLIBERA_BUILD_TESTS=OFF \
  -DLIBERA_BUILD_LASER_TOOL=OFF

cmake --build --preset release
```

Output: `/tmp/libera-laser/build/release/liblibera-core.a`

## 2 — Build circle_daemon

```sh
cd /path/to/BeamCommander3/daemon
bash build.sh
```

`build.sh` auto-detects the libusb path via `pkg-config`. You can override the libera-laser location:

```sh
LIBERA_DIR=/path/to/libera-laser bash build.sh
```

The raw compiler command (for reference):

```sh
clang++ -std=c++17 -O2 \
  -I/tmp/libera-laser/include \
  -I/tmp/libera-laser/libs/asio/include \
  -I/tmp/libera-laser/libs/helios_dac/sdk/cpp \
  circle_daemon.cpp \
  /tmp/libera-laser/build/release/liblibera-core.a \
  $(pkg-config --cflags --libs libusb-1.0) \
  -framework CoreFoundation -framework IOKit \
  -framework CoreAudio -framework AudioToolbox \
  -lpthread \
  -o circle_daemon
```

## 3 — Run

```sh
./circle_daemon --ip 10.10.10.4 -h   # show help
./circle_daemon --ip 10.10.10.4      # stream with defaults
```

## Architecture

```
circle_daemon.cpp
  └── libera-laser (C++17 static lib)
        ├── EtherDreamManager  — UDP discovery + TCP streaming
        ├── HeliosManager      — USB (libusb)
        ├── LaserCubeNet       — network LaserCube
        └── …
```

- Discovery: libera broadcasts UDP on port 7654 and collects responses for ~3 s.
- Connection: TCP to port 7765 (Ether Dream). libera handles the full handshake (ping → firmware probe → prepare → begin → write_data loop).
- Frame mode: `isReadyForNewFrame()` / `sendFrame()` — libera handles buffer management internally.
- `--ip` uses `dynamic_cast<EtherDreamControllerInfo*>` to filter discovered controllers by IP before connecting.

## Python backend / web frontend

The `backend/` (FastAPI) and `frontend/` (Vue 3 + Vite) directories provide a browser-based simulation preview only — they do **not** output to hardware.

```sh
# Backend
cd backend
python -m venv .venv && .venv/bin/pip install fastapi uvicorn[standard]
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000

# Frontend
cd frontend
npm install
npm run dev -- --port 5173 < /dev/null   # < /dev/null prevents Vite tty-suspend
```

## After every major change

- Update `README.md` user-facing sections (options table, quick start).
- Update `DEVELOPER.md` build steps if dependencies or flags change.
