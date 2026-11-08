# Developer Guide

## Project layout

```
backend/
  laser_daemon.cpp   Single C++ backend: libera-laser output + HTTP + WebSocket
  output_pipeline.h  Hardware output safety pipeline: blackout/arm gate, zone, safety masks
  param_smoothing.h  Soft transitions of show parameters (scale, speeds, move size, ...)
  show_fx.h          Flicker rate + on time
  json_lite.h        Tiny JSON field readers shared by the daemon and the tests
  tests/             Backend unit tests, run before every push (.githooks/pre-push)
  httplib.h          Vendored header-only HTTP/WebSocket server (yhirose/cpp-httplib)
  build.sh           Compiles laser_daemon (and legacy circle_daemon)
frontend/
  src/components/ControlPanel.vue   All UI controls (shape, color, transform, FX...)
  src/components/CuePanel.vue       Cue grid (save/recall/move/clear)
  src/components/ZoningPanel.vue    Drag-to-move/scale per-laser pan/zoom calibration
  src/components/LasersPanel.vue    Add/remove lasers (DACs), arm/disarm each one
  src/components/LaserScene.vue     Three.js 3D preview, renders WS point stream
  src/components/TouchDock.vue      Bottom dock: opens panels, per-tab preview toggle
  src/components/FloatingPanel.vue  Shared draggable/resizable/maximizable panel wrapper
  src/components/maskGeometry.js    Safety mask editing geometry (+ maskGeometry.test.js)
  src/components/showFx.js          Preview flicker, same formula as backend/show_fx.h (+ showFx.test.js)
  src/composables/useLaserSocket.js REST + WebSocket client, shared reactive state
start.sh             Builds backend if needed, runs both processes, cleans up on Ctrl-C
.githooks/pre-push   Runs all unit tests before every push (git config core.hooksPath .githooks)
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

## Persistent user data (midi_map.json, cues.json, magic_touch.json, lasers.json, settings.json)

`laser_daemon` never reads/writes these files at whatever relative path
happens to be current - it always resolves them to a per-OS, per-user
app-data directory, completely independent of cwd or where the binary/app
itself lives:

- macOS: `~/Library/Application Support/BeamCommander3/`
- Windows: `%APPDATA%\BeamCommander3\`
- Linux: `$XDG_CONFIG_HOME/BeamCommander3/` (or `~/.config/BeamCommander3/`)

It prints this directory on every startup (`[laser_daemon] User data
directory: ...`), so it's always obvious where settings/cues are actually
coming from. On first run, if a file doesn't exist yet there, it's seeded
by copying a same-named default shipped alongside the app (checks next to
the executable, and - for the macOS `.app` bundle layout specifically,
where `Contents/MacOS/laser_daemon` and `Contents/Resources/midi_map.json`
aren't in the same directory - a sibling `Resources/` folder too).

### Shipped defaults (sync before every release)

Releases ship the maintainer's own `cues.json` and `magic_touch.json`
(committed in `backend/`, next to `midi_map.json`) as first-run defaults, so new
users start with useful cues and a ready-made Magic Touch layout instead of
an empty app. Since every push to `main` publishes a release, run this
before pushing and commit whatever it changed:

```sh
packaging/sync_default_data.sh   # copies them from the app data dir into backend/
```

`settings.json` holds the central Beam settings (scan rate, max rate, output
latency, dot density/dwell/blanking, flicker): loaded at startup
(`load_settings_from_disk()`), written by a small background thread within
~1 s of any change (`save_settings_if_changed()`). Like `lasers.json` it is
tuned to this rig's projectors and not shipped.

`lasers.json` is intentionally not shipped (it holds this rig's DAC IP
addresses), and `midi_map.json` isn't synced because the app never writes
it - edit `backend/midi_map.json` in the repo directly. Existing users are unaffected: seeding only happens for files
that don't exist in their data directory yet.

This replaced an earlier CWD-relative-path approach that had a real bug:
`start.sh` used to launch `laser_daemon` from the repo root instead of
`backend/`, which silently disabled MIDI mapping (`"no mapping file...
MIDI control disabled"`) and could've read/written a stray `cues.json` in
the wrong directory - with no obvious error. See `exe_dir()`,
`app_data_dir()`, and `resolve_data_file()` in `laser_daemon.cpp`.

`lasers.json` (the configured laser/DAC list, including each one's `armed`
state and its own zone_x/zone_y/zone_scale_x/zone_scale_y pan/zoom
calibration - see "Lasers (multi-DAC output)" below) lives in the same
per-user app-data directory and is seeded/resolved the same way, via
`load_lasers_from_disk()`/`save_lasers_to_disk()`.

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
handles the frontend instead, is unaffected). This is the mechanism behind
the packaged macOS releases published by
`.github/workflows/release.yml`: it builds `laser_daemon` and the
frontend, wraps them into a proper `BeamCommander3.app` bundle (with the
Homebrew `libusb` dylib bundled + re-signed so it doesn't require Homebrew
on the target machine), and ships a `.dmg` with a drag-to-Applications
shortcut. See `packaging/launcher.sh` and `packaging/Info.plist.template`
for the app bundle's launcher script and metadata.

## README screenshots

The screenshots in [README.md](README.md#screenshots) (`docs/screenshots/*.png`)
are generated, not hand-captured, and committed straight into the repo so
they render even when README.md is viewed offline/from a clone. Regenerated
automatically by the `build-macos` job of `.github/workflows/release.yml`
on every push to `main`:

1. Builds `laser_daemon` and `frontend/dist` as usual, then runs the real
   backend against the built frontend (`FRONTEND_DIST=... ./laser_daemon`) -
   no mocking, so the screenshots reflect an actually-working app.
2. Applies a colorful demo look via `POST /api/state` and saves it into cue
   slot 1 (`backend/cues.json` is gitignored/local-only runtime data, so a
   fresh checkout has no saved cues to rely on) and adds one example
   (unarmed) laser so the Lasers/Zoning panels aren't empty placeholders.
3. `frontend/scripts/screenshots.mjs` (Playwright) drives a headless
   Chromium at a fixed 1440×900 viewport (the default MacBook Air
   resolution, kept fixed so image dimensions never drift release to
   release) and captures one shot of the idle preview plus one per dock
   menu button (Settings/Cues/Zoning/Lasers).
4. The workflow commits the resulting PNGs back to `main` as
   `docs: update screenshots [skip ci]`. The `on.push.paths-ignore` filter
   for `docs/screenshots/**` stops that commit from re-triggering this same
   workflow.

To regenerate locally: build the backend and frontend as above, `cd frontend
&& npx playwright install chromium`, start `laser_daemon` pointed at
`frontend/dist` via `FRONTEND_DIST`, then run
`BASE_URL=http://localhost:8000 OUT_DIR=../docs/screenshots node scripts/screenshots.mjs`.

## Building for Windows

macOS keeps using `build.sh` (raw `clang++`) as shown above. Windows instead
builds through CMake, using `backend/CMakeLists.txt` (only used for this -
not needed/used on macOS), which pulls in libera-laser via `add_subdirectory`
so all its library requirements (libusb, Winsock, etc.) come along
automatically instead of having to hand-list include/lib paths:

```powershell
git clone --depth=1 https://github.com/sebleedelisle/libera-laser.git C:/libera-laser
cd C:/libera-laser
git submodule update --init --recursive
cd ../beamcommander3

cmake -S backend -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIBERA_DIR=C:/libera-laser
cmake --build build
```

Needs a Ninja + MSVC environment (e.g. via `ilammy/msvc-dev-cmd` in CI, or a
"Developer PowerShell for VS" prompt locally). Produces `build/laser_daemon.exe`.
Windows has no system package manager for libusb, so this uses
`-DLIBERA_USE_BUNDLED_LIBUSB=ON` internally (set automatically for `WIN32` in
`backend/CMakeLists.txt`) - libera-laser ships a prebuilt libusb DLL in its
vendored `helios_dac` submodule
(`libs/helios_dac/sdk/cpp/libusb_bin/Windows/x64/Release/dll/`), which needs
to sit next to `laser_daemon.exe` at runtime (the release workflow copies it
into the package - see `packaging/run_windows.bat` for the launcher).

CoreMIDI (and therefore the optional MIDI/footswitch control layer) is
macOS-only - `laser_daemon.cpp` already guards this behind `#ifdef __APPLE__`
and simply prints "MIDI control disabled" on other platforms instead of
failing to compile; everything else (REST API, WebSocket preview, laser
output, cues) works the same on Windows.

The release workflow packages `laser_daemon.exe` + the bundled libusb DLL +
`midi_map.json` + `frontend/dist` + `packaging/run_windows.bat` into a real
installer, **`BeamCommander3-Setup.exe`**, built with
[Inno Setup](https://jrsoftware.org/isinfo.php) (preinstalled on GitHub's
`windows-latest` runners - no extra install step needed) via
`packaging/windows_installer.iss`:

```powershell
& "C:/Program Files (x86)/Inno Setup 6/ISCC.exe" `
  /DMyAppVersion=v2026.07.05-1 `
  /DStageDir=C:/path/to/staged/files `
  /DIconFile=packaging/AppIcon.ico `
  packaging/windows_installer.iss
```

`StageDir` must contain everything above; see the release workflow's
"Assemble release package (installer)" step for how it's assembled. The
installer installs per-user (no admin rights required, via Inno Setup's
`{autopf}`/`PrivilegesRequired=lowest`), adds Start Menu + optional desktop
shortcuts pointing at `run.bat`, and offers to launch the app immediately
after install.

`.github/workflows/release.yml` builds both platforms in parallel jobs and
publishes them (the macOS `.dmg` and `BeamCommander3-Setup.exe`) as two
assets on the same GitHub Release.

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
  │       regardless of any hardware connection state
  │     - owns one core::LaserController per configured+assigned laser
  │       (G_lasers) and, each tick, sends the same zone-transformed frame
  │       to every one of them that's connected + ready, in parallel - see
  │       "Lasers (multi-DAC output)" below
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
- **Connect retry backoff.** Each laser's hardware connect attempts back off
  1s after a failure (`LaserRuntime::next_connect_attempt`) so a
  persistently-unreachable IP doesn't spam TCP connect attempts on every
  loop iteration - tracked independently per laser, so one unreachable DAC
  never delays connecting to (or streaming) the others.
- **Single non-recursive mutex (`G_mtx`).** Every REST handler that mutates
  `LaserState` must fully release the lock *before* calling `state_to_json()`,
  which locks the same mutex again. Getting this wrong deadlocks the handler's
  worker thread permanently — cpp-httplib's thread pool is small, so a few of
  these in a row makes the whole server unresponsive (looks like a crash, but
  the process is actually just stuck). Always scope `lock_guard` in its own
  `{ }` block or inside a `try { }` that ends before the `res.set_content(...)`
  call.
- **`POST /api/reset`** restores `LaserState` to its default-constructed
  values but preserves `flash_release_ms`/`max_rate_kpps`/`blackout`
  (all global daemon settings - see "Cue system" below). The configured
  laser list, including each laser's `armed` state and zone calibration
  (`G_lasers`/`lasers.json`), is entirely separate from `LaserState` and
  untouched by a reset.
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

`G_rainbow_phase` is the one exception to the wrapping: it is *not* kept in
0..1, and it is held at 0 while `rainbow_amount` is 0. `make_frame()` takes
the hue position as a parameter and adds it to the beam color's own hue, so
switching the rainbow on always starts at the current color (saturation and
brightness also glide from the beam color to full over the first 10% of
`rainbow_amount`). The unwrapped value lets `laser_thread()` run
`rainbow_phase_degrees` as a delay chain: laser *i* gets
`max(0, G_rainbow_phase - i * step)`, i.e. the same animation, *i* steps
later, never earlier than the switch-on point. The spatial bands use
`hue = phase - x * cycles`, so they travel towards higher x (left to right on
lines/waves) - the same direction the chain passes colors between lasers.

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

### Soft parameter transitions

Everything that changes a show parameter (REST, MIDI, Magic Touch, reset)
only sets the *target* in `G` and glides. **Cue recall and momentary cues
are the exception**: they bump `G_show_snap_seq`, and `laser_thread()` then
jumps every glide (`snap_all`, plus `G_pos_*_smooth` and the wave knobs)
straight to the new look in the same tick - switching cues must be instant.
Global settings survive look changes via `with_global_settings()`.

The other part of a cue switch's delay is libera's frame queue: frames are
stamped `now + targetLatency` and that much is kept queued. libera's
default is 100 ms; `output_latency_ms` (global setting, default 40 ms,
10-200) is applied via `LaserController::setTargetLatency()` whenever it
changes.

`laser_thread()` owns a
`ShowParamSmoother` (`param_smoothing.h`) that glides the rendered values
towards those targets every tick and writes them into the snapshot passed
to `make_frame()` (`smooth.apply(snap)`), so `GET /api/state` and the UI
still show the target immediately.

- Exponential glide (`glide_toward`), frame-rate independent and the same
  duration for any jump size; snaps exactly onto the target at the end
  (the renderer checks `rainbow_amount > 0` / `dot_amount < 1`).
- `radius`, `shape_scale`, `move_size`, `rainbow_amount`, per-laser
  `move_phase_degrees`/`rainbow_phase_degrees`/`flicker_phase_degrees`:
  tau 0.15 s (~0.45 s to 95 %).
- `dot_amount`: tau 0.35 s (~1 s), so dots visibly fade in/out one by one
  (see "Dot rendering").
- `move_speed`, `rotation_speed`, `rainbow_speed`: tau 0.25 s, and the phase
  accumulators integrate the *smoothed* speeds, so the motion accelerates/
  decelerates instead of changing rate abruptly.
- Changing `move_mode` crossfades the old and new movement offset over
  0.45 s (smoothstep) via the render-only `move_mode_from`/`move_blend`
  fields in `LaserState`.
- Motion hold and rotation reset bump `G_speed_snap_seq`, which makes the
  smoother take the new speeds instantly - a freeze stays a freeze.
- Not smoothed on purpose: color, intensity, flash, blackout, shape/point
  count, flicker, scan rate. `pos_x`/`pos_y` and the wave knobs keep their
  own adaptive glide (`smooth_toward`, below).

### Flicker (rate + on time, exact per point)

`flicker_hz` (rate) and `flicker_on_ms` (lit time per flash, 0 = auto =
half the period) are *not* applied in `make_frame()`. Deciding on/off once
per frame made the laser (frames looped by the DAC, replaced every >= 20 ms)
and the preview (one frame per 33 ms) quantise the rhythm differently, so
they visibly drifted apart. Instead (`show_fx.h`):

- Laser: in the hardware-send step each frame is padded by repeating the
  closed shape to >= 30 ms (`pad_frame_to_duration`, max 3000 points) so the
  DAC plays it once instead of looping it, and every point is blanked by its
  playback time on a continuous per-DAC clock (`FlickerTimeline`, anchored
  to `G_time`, re-anchored after > 250 ms drift) via `flicker_is_lit()`.
- Preview: WS messages carry the unflickered frames plus `t` (`G_time`) and
  `fl` (`[flicker_hz, flicker_on_ms]`); `LaserScene.vue` applies the same
  formula (`showFx.js`) per simulated point, on the daemon's clock
  (`createClockSync`), so both blink in the same rhythm.
- Flicker phase (`flicker_phase_degrees`): laser *i* (Lasers-list order)
  runs its flicker `i * deg/360` of a period later
  (`flicker_phase_offset`) - added to its hardware timeline and sent to the
  preview per laser as `flo` (`{id: seconds}`).
- Mask outlines are appended after flicker, so they stay steady.

### Zoning (per-laser pan/zoom calibration)

`zone_x`/`zone_y`/`zone_scale_x`/`zone_scale_y` on each `LaserConfig` (see
"Lasers (multi-DAC output)" below) are a master pan/zoom applied on top of
everything else (shape/position/rotation/movement/mirror) — for mapping the
whole show onto a physical sub-region of *that specific laser's* range
(e.g. a wall or truss segment), not a per-look show parameter. Each
configured laser has its own independent calibration, since two projectors
in the same rig often need different alignments even while showing the
exact same content. Set only by dragging the box in `ZoningPanel.vue` (no
numeric fader by design), which lets the operator pick which laser to
calibrate from a dropdown, backed by `POST /api/lasers/<id>`.

- **Applied after `make_frame()`, per laser,** in
  `finalize_hardware_frame()` (`output_pipeline.h`), together with
  blackout, the footswitch gate, mirror and masks. The WS preview goes
  through the exact same call for every laser (without blackout), so it
  shows what the laser really outputs - see "Preview = real output" below.
- **Independent per-axis scale**, not a single uniform zoom, so the zone
  can be stretched/squeezed on just one axis to match a non-square venue
  region.
- Points the zone pushes outside the DAC's ±1 range are blanked (not
  clamped), same reasoning as `make_frame()`'s own off-screen handling —
  clamping would pin/smear the shape against the venue boundary instead of
  cleanly cutting it off.
- Persisted as part of each laser's own entry in `lasers.json` (not
  `cues.json`) — see "Lasers (multi-DAC output)" below.
- Excluded from cue state and `/api/reset` for the same reason as
  `blackout`/`flash_release_ms` — see "Cue system" below.

### Preview = real output

Every WS preview message carries one frame per configured laser
(`laser_pts`), each run through the same `finalize_hardware_frame()` as
that laser's DAC copy: master brightness gate/flash fade, mirror X/Y, zone,
safety masks and mask outlines all show in the preview exactly as on the
laser. The one deliberate exception is **blackout**: the preview is built
with `blackout = false`, so the look stays visible (and can be prepared)
while the real output is blacked out. "Armed" is the configured state there, so an armed but not
yet connected laser previews what it will show; disarmed beams are hidden
by `LaserScene.vue`. Without any configured laser, the main `pts` frame
(virtual preview laser) still gets the gate. `LaserScene.vue`
therefore must not mirror or dim frames itself.

### Safety masks (per laser)

Like the beam masks in Pangolin Beyond/Liberation: each `LaserConfig` has
a `masks` list of `LaserMask` rectangles (`output_pipeline.h`), edited in the
Zoning panel's **Masks** mode and set via `POST /api/lasers/<id>/masks`
(always the complete list).

- **Output space, after the zone.** Mask coordinates are the laser's
  physical -1..1 range, applied after mirror + zone pan/zoom, so a mask stays
  on the same spot in the room however the content or zone moves. The
  ZoningPanel stage is that same range (the green zone box and red masks
  share it).
- **Reduction.** Light inside is multiplied by `1 - reduce/100`; overlapping
  masks use the strongest reduction. Borders count as inside.
- **Exact cuts.** `apply_masks()` splits every lit segment where it crosses
  a mask border (emitting the crossing point twice, before/after
  brightness), so sparse shapes (a 4-corner square, a low-point line) can't
  draw straight through a mask between two points outside it. Splits are
  capped at `kMaskFrameMaxPoints` (3800); past that, per-point dimming
  still applies.
- **Red setup outline.** `outline` makes `append_mask_outlines()` add the
  border in red (fixed `kOutlineRed`, independent of intensity/footswitch)
  after the masked show, with blanked, interpolated travel and corner
  dwell. Only drawn when the laser is armed and not blacked out. Runtime-
  only: never written to `lasers.json`, and the panel switches it off on
  unmount/`pagehide` (with `keepalive` fetches). Edge density shrinks to stay
  under 4000 points per frame (Ether Dream buffer: 4095); if even that
  doesn't fit, no outline is drawn.
- **Order** (`finalize_hardware_frame()`): blackout/not armed → dark; else
  footswitch fade → mirror + zone (off-range blanked) → masks → outlines.
  `laser_thread()` builds `HardwareOutputParams` via `hardware_params()`
  and calls it for each DAC copy *and* each laser's preview frame.
- **Validation.** `parse_masks()` refuses the whole list on malformed JSON or
  non-finite numbers (`std::stof` accepts `nan`, which would silently
  disable a mask), clamps to -1..1 / 0..100, orders corners and caps at 64.
  A `lasers.json` entry with unreadable masks logs a loud warning.

### Lasers (multi-DAC output)

`G_lasers` (`std::vector<LaserConfig>`, guarded by `G_lasers_mtx`) is the
configured list of physical DACs, persisted to `lasers.json` and exposed
via `GET/POST /api/lasers`, `POST /api/lasers/<id>`, `DELETE
/api/lasers/<id>` — added/renamed/re-IP'd/removed/armed/calibrated from the
**Lasers** panel (`LasersPanel.vue`) and **Zoning** panel (`ZoningPanel.vue`)
in the UI. Each entry is `{id, name, ip, armed, zone_x, zone_y,
zone_scale_x, zone_scale_y}`; `armed` is the master enable for that
specific laser - `false` means configured but idle (no network connection
ever attempted), `true` means `laser_thread()` connects and streams to it,
transformed by *that laser's own* zone calibration.

`laser_thread()` owns a `std::map<int, LaserRuntime>` — one
`core::LaserController` + its own connect/pacing state per laser id — kept
entirely local to that thread (never shared with the HTTP thread; only a
plain `bool` "connected" summary is, via `G_laser_connected`/
`set_laser_connected()`, which `GET /api/lasers` reads). Every tick it:

1. Snapshots `G_lasers` (brief lock, no HTTP request ever blocks on
   connect/send work).
2. Drops runtime state for any laser id no longer configured at all.
3. Connects/disconnects/retries each laser independently based on its own
   `armed`/`ip` (same connect-retry-backoff logic as before, just
   duplicated per laser instead of a single global `ctrl`).
4. If *any* armed+connected laser is ready for a new frame, renders one
   blackout/gate-applied base frame and fans it out to every laser that's
   currently ready (`isReadyForNewFrame()` + its own 20ms pacing floor),
   applying that specific laser's own zone_x/zone_y/zone_scale_x/
   zone_scale_y transform to its copy just before sending.

This works safely in parallel (not sequentially/one-at-a-time) because
`core::LaserController::sendFrame()` just queues the frame into that
controller's own internal scheduler rather than blocking on the network
write — looping over N controllers and calling `sendFrame()` on each ready
one therefore adds no meaningful per-controller latency, and one slow/
unreachable DAC can never delay or lag another's output.

### Cue system

A cue also stores the rotation *angle* at save time (`rotation_phase`, read
from `G_rotation_phase`). Recall doesn't write the angle directly: it calls
`request_rotation_angle()` under `G_mtx`, and `laser_thread()` applies the
angle and snaps the smoothed rotation speed (`ShowParamSmoother::snap_rotation`)
in the same tick - so a recalled look starts exactly at its saved angle
instead of being turned further by the previous look's still-gliding
rotation. Rotation reset uses the same path with angle 0. Momentary cues
(`do_cue_momentary_press/release`, MIDI and `POST /api/cue/<n>/momentary/<1|0>`)
remember and restore the angle too.

Cues are snapshots of `LaserState` in `std::map<int, LaserState> G_cues`,
guarded by its own `G_cues_mtx` and persisted to `backend/cues.json`. A
handful of fields are deliberately *not* part of that snapshot even though
they live on `LaserState` - `flash_release_ms`, `intensity`
(master brightness), `blackout` and all Beam settings (`rate_kpps`,
`max_rate_kpps`, `output_latency_ms`, `dot_amount`, `dot_dwell_us`,
`dot_blank_us`, `flicker_hz`, `flicker_on_ms`, `flicker_phase_degrees`) are
global daemon/safety settings, not per-look show state (one place for this:
`with_global_settings()`): they're excluded from
`cue_state_to_json`/`cue_state_from_json` and explicitly re-applied on top
of `G` after every `do_cue_recall()` (and preserved across `/api/reset` the
same way), so switching cues or resetting the show can never change the
footswitch fade time, the master brightness fader, whether the laser is
blacked out, or the configured scan-rate ceiling. Each laser's own
connection/zone calibration lives entirely outside `LaserState` (in
`G_lasers`/`lasers.json` instead), so it's naturally unaffected by cues too
- see "Lasers (multi-DAC output)" above.

**`G_mtx` and `G_cues_mtx` are never held at the same time** -
`do_cue_save/recall/clear()` in laser_daemon.cpp are the *only* functions
allowed to touch `G_cues`, and each one locks at most one of the two
mutexes at a time (lock, copy/assign, unlock - never nested).
Both the `/api/cue/*` HTTP routes and the MIDI dispatcher's `cue:<n>` /
`cue_momentary:<n>` actions call these same helpers instead of duplicating
the logic, so there's exactly one place that has to get the lock order
right. If you add a new cue-related code path, route it through these
helpers rather than touching `G_cues` directly.

The frontend's cue grid (`CuePanel.vue`) also renders a small animated SVG
preview icon per populated slot straight off the saved state object (shape,
color, rotation, movement, rainbow, dot pattern) - see that file's
`cue*Style()` helper functions if you add a new animatable parameter that
should show up there too. Two gotchas already hit while building it:
`<style scoped>` rewrites `@keyframes` names, which breaks any animation
name built dynamically in JS (put those keyframes in a second, unscoped
`<style>` block instead), and `linearGradient` needs
`gradientUnits="userSpaceOnUse"` - the default `objectBoundingBox` silently
fails to paint on a shape with a degenerate (zero width/height) bounding
box, like a horizontal `<line>`.

### Dot rendering (dot_amount < 1)

Dot timing is in microseconds, not points (`dot_timing()`, `show_fx.h`):
before each dot the beam stays dark for "Dot blanking" (`dot_blank_us`,
default 300 µs) plus 2 ms per unit of jump distance (max 4 ms) so the
galvos can travel and settle, the dot is lit for "Dot dwell"
(`dot_dwell_us`, default 400 µs), then dark again for `dot_blank_us` (the
laser's own switch-off delay) before the galvos move on. Converted to points
at the actual scan rate. The old fixed point counts (~0.13 / 0.07 ms at
30 kpps) made every dot a streak on real projectors. Both settings are
global (not in cues, kept across recall/reset, also used by cue layers).
With many dots the times are compressed to fit the point budget, never
below 2 settle + 2 lit + 1 trail points.

The dot count is continuous (`dot_amount * n`, `place_dots()` in
`show_fx.h`): dots sit evenly spaced at fractional base-point positions
(interpolated between base points; closed shapes wrap, lines/waves don't),
and with a fractional count the newest dot is drawn at that fraction's
brightness. Together with the slower `dot_amount` glide, turning the knob
makes the dots slide apart/together continuously while new dots fade in (or
out) - instead of every dot jumping to a new position each time the
whole-number count changed.

`make_frame()` doesn't just thin the point list by an integer stride - a
DAC plays a frame's points back-to-back at a fixed rate, moving the galvo
in a straight line between consecutive buffer entries (it does not
teleport), so naively dropping points would just redraw the same solid
outline in hardware even though the browser preview (which draws each
point as its own isolated ray) looked correctly dotted. Instead:

- Kept dot indices are spaced evenly by dividing the point count in
  floating point (not a fixed integer stride), so gaps - including the
  wrap-around pair - come out equal instead of dumping all the rounding
  error into one gap.
- Each kept dot is surrounded by blanked settle points before it goes lit,
  scaled by how far the galvo actually has to jump (a real mirror needs
  time to stop moving after a big jump - too little settle time and dots
  come out as short streaks instead of clean points).
- Any point that would land outside the +-1 safe range on either axis is
  blanked outright, rather than letting the downstream DAC encoder
  clamp-and-draw it at the boundary (which flattens an oversized/moved
  shape against the screen edge - e.g. a large circle degenerating into a
  rectangle).

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

## Cue layers

A cue can play *on top of* the main look (`G`) instead of replacing it:
`G_show_layers` (max 8, guarded by `G_mtx`) holds a copy of each layered
cue plus its own move/rotation/wave/rainbow phases, integrated in
`laser_thread()` (frozen by motion hold via `G_motion_frozen`). Rendering:
`make_frame()` takes a `RenderPose` (rotation, position, wave, point budget)
instead of reading globals; `base_pose()` builds it for the main look,
`layer_pose()` for a layer. Every look gets `kFramePointBudget / (1 + layers)`
points, is rendered per laser by `render_look_for_laser()` (per-laser
phase offsets), and the figures are joined by `compose_figures()`
(`show_fx.h`): each figure closed, blanked interpolated travel + settle
between figures and back to the start. Master brightness and a held flash
apply to layers (`layer_render_state()`); blackout, gate, zone, masks and
flicker act on the composed frame. Solo recalls keep layers; `/api/reset`
and `POST /api/layers/clear` remove them. API: `POST
/api/cue/<n>/layer/<on|off|toggle>`; MIDI `cue_layer:<n>`,
`cue_layer_flash:<n>`, `layers_clear`. Magic Touch: cue buttons with
`w.layer` (Playback: Layer) and `w.momentary` (Flash).

## Magic Touch multitouch

`MagicTouchBuilder.vue` tracks every finger as its own gesture, keyed by
`pointerId` (`gestures` for faders/knobs/X/Y pads, `held` for held
momentary buttons), and each control captures its own pointer - so several
controls can be operated at once. Two-finger panning in run mode only
counts fingers that start on free canvas space (`CONTROL_SELECTOR`); in edit
mode any second finger pans and cancels an element drag. Several held
momentary cues stack (`heldMomentaryCues` in `useLaserSocket.js`): the
newest shows, releasing falls back to the previous one, the last release
restores the original look; requests are chained so the daemon sees them in
order.

Testing: Chromium's CDP touch emulation never sends `pointerup` when a single
finger of several is lifted, and synthetic `PointerEvent`s can't be
captured - for automated checks dispatch synthetic touch `PointerEvent`s with
`Element.prototype.setPointerCapture` stubbed out.

## Tests (run before every push)

Every `backend/tests/test_*.cpp` is a self-contained test program (no
framework, header-only - needs only libera's include dir):

- `test_output_pipeline.cpp` - the hardware output safety pipeline
  (`output_pipeline.h`: safety masks, outlines, blackout/arm gate, mirror,
  zone transform, mask JSON), including a randomized test that checks over
  20,000 random frames/mask sets that no lit segment lands inside a mask.
- `test_param_smoothing.cpp` - soft parameter transitions
  (`param_smoothing.h`).
- `test_show_fx.cpp` - flicker rate + on time, the per-point flicker
  timeline and composing several looks into one frame (`show_fx.h`).

Tests are **not** part of the build (`build.sh`, `npm run build`,
CMake) - they run before every `git push` via the versioned hook
`.githooks/pre-push`, which aborts the push if any test fails. Activate it
once per clone:

```sh
git config core.hooksPath .githooks
```

- Backend: `backend/tests/run_tests.sh` compiles and runs every
  `tests/test_*.cpp` with `-Wall -Wextra -Werror` and
  UndefinedBehaviorSanitizer (`TEST_SANITIZERS=address,undefined` adds
  ASan - not the default, ASan hangs even on an empty program with some
  macOS/toolchain combinations; `CXX` picks the compiler).
- Frontend: `npm test` (`node --test`, no extra dependencies) runs
  `maskGeometry.test.js` and `showFx.test.js` (the preview's flicker must
  match `show_fx.h`).
- CMake (Windows): `-DBEAMCOMMANDER_TESTS=ON` builds the test programs,
  `ctest --test-dir build` runs them. Add new test files to the `foreach`
  list in `CMakeLists.txt`.
- Run everything manually: `.githooks/pre-push`.

The release workflow (`.github/workflows/release.yml`) does not run the
tests; `git push --no-verify` skips the hook.

## After every major change

- Update `README.md` — endpoint table, quick start, user-facing behavior.
- Update this file — architecture notes, build steps, gotchas.
