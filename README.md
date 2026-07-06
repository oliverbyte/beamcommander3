# BeamCommander3

A laser show controller you run on your own laptop: it drives a real laser
projector and gives you a live web page to control shapes, colors, movement,
color effects, and "cues" (saved looks you can jump back to instantly) —
plus an optional USB MIDI controller or foot pedal if you want hands-on
faders and buttons instead of a mouse.

You don't need to know how to code to use it — see
["Using the show"](#using-the-show-no-coding-needed) below. If you're
curious how it's built, or want to modify it, the rest of this file and
[DEVELOPER.md](DEVELOPER.md) cover the technical side.

## History

BeamCommander3 is the third generation of a laser-show controller originally
built as **BeamCommander**, an [openFrameworks](https://openframeworks.cc/)
(C++ creative-coding framework, "ofx" for short) application. That version
worked, but every change meant recompiling a full openFrameworks project,
and the laser output, the UI, and the MIDI/OSC control layer were all
tangled together in one big app.

This rewrite keeps the parts that worked well from the original — the exact
Akai APC40 mkII MIDI mapping, the response-curve/smoothing math, the flash
and cue behavior — and drops openFrameworks entirely. The laser output now
talks directly to [libera-laser](https://github.com/sebleedelisle/libera-laser),
a small, focused C++ library just for driving laser hardware (Ether Dream,
Helios, LaserCube, and others). The control surface is a normal web page
(Vue 3 + Three.js) served over plain HTTP/WebSocket, so it opens in any
browser and can be styled or extended without touching any C++ at all. The
result is one small backend program plus one web page — no openFrameworks
project, no IDE-specific build system, nothing except a C++ compiler and
Node.js.

## Installing

Grab the latest build from the [Releases page](../../releases) — no
compiler, Node.js, or source checkout needed.

### macOS

1. Download the `.dmg` from the latest release.
2. Open it, drag **BeamCommander3** into **Applications**.
3. Open it from Applications. It's not notarized (no Apple Developer ID),
   so the first time macOS will refuse it as "from an unidentified
   developer" — right-click the app and choose **Open** once to approve it
   (only needed the first time).
4. A browser tab opens automatically at `http://localhost:8000` once the
   backend is ready — that page is the remote control.

### Windows

1. Download `BeamCommander3-Setup.exe` from the latest release and run it.
   It's not code-signed, so SmartScreen may warn — click **More info** →
   **Run anyway**.
2. Installs to your user profile (no admin rights needed), adds Start
   Menu/optional desktop shortcuts, and offers to launch the app right
   after install.
3. A browser tab opens automatically at `http://localhost:8000`.

### Manual / from source (any OS, or for development)

```sh
git clone https://github.com/oliverbyte/beamcommander3.git
cd beamcommander3
./start.sh
```

This builds the backend if needed and runs both the backend and a live
frontend dev server, opening a browser tab automatically. Requires the
[developer prerequisites](DEVELOPER.md) (a C++ compiler, CMake, Node.js,
and a built copy of libera-laser) — see DEVELOPER.md for the full setup and
for how the packaged macOS/Windows builds are put together.

## Using the show (no coding needed)

1. **Start it**: use one of the options in ["Installing"](#installing)
   above. Whichever one you pick, it opens a browser tab automatically —
   that page *is* the remote control for your laser show.
2. **Connect your laser**: type its IP address into the "Controller" box in
   the UI and click **Connect** (e.g. an Ether Dream is often
   `192.168.x.x` or a fixed address like `10.10.10.4` — check your laser's
   manual or network settings). You'll see "Laser → connected" once it's
   linked up. No laser connected yet? Everything still works — the preview
   on screen always shows exactly what would come out of the laser.
3. **Play with the show**: pick a shape, drag the color picker, turn on
   Rainbow, try a Movement pattern, adjust Speed/Size — every change is
   instant, nothing needs to be "applied" or saved first.
4. **Flash / Blackout**: the ⚡ Flash button punches the beam to full white
   brightness while held, then returns to whatever it was showing before.
   Blackout forces the real laser dark (the on-screen preview keeps
   playing) — handy for a quick "oh no" moment without losing your settings.
5. **Cues**: the panel on the right holds 32 save slots. Turn on "Save
   mode", click an empty numbered slot to store the current look, then
   click any populated (green) slot any time to instantly recall it. Each
   populated slot shows a small live preview icon of its shape, color and
   motion (rotation, movement, rainbow, wave, dotted look) so you can see
   what a cue looks like before recalling it. Right-click a slot to clear
   it, or use "Move" to relocate a saved cue to a different slot.
6. **Optional: MIDI controller or foot pedal** — if you plug in a USB MIDI
   controller (an Akai APC40 mkII works out of the box, matching the
   original BeamCommander's exact layout), its knobs and buttons drive the
   same controls as the web page. A foot pedal can be wired up as a
   "brightness on while held" switch. None of this is required — the web
   page alone is a complete remote control. See ["MIDI control"](#midi-control-optional)
   below if you want to set one up.

---

## For developers

Everything below is technical reference for building, extending, or scripting
against BeamCommander3 — not needed just to run a show (see above).

## Architecture

```
start.sh
  ├── backend/laser_daemon   C++: libera-laser output + HTTP :8000 + WebSocket
  └── frontend/              Vue 3 + Vite + Three.js UI, talks to :8000
```

There is no Python and no separate daemon process — `laser_daemon` *is* the
backend. It generates every frame once, streams it to the browser preview via
WebSocket, and (if a controller is connected/armed) sends the same frame to
the hardware. Preview and hardware output are decoupled: the preview keeps
animating and responding to every parameter change even when no laser is
connected.

## Quick start

```sh
./start.sh
```

This builds `backend/laser_daemon` if needed, starts it plus the Vite dev
server, and opens up:

- UI: http://localhost:5173
- API: http://localhost:8000/api/state

Press `Ctrl-C` to stop both processes cleanly.

## Connecting a real laser

In the UI, enter the controller's IP address (e.g. an Ether Dream at
`10.10.10.4`) and click **Connect**. Or via the API:

```sh
curl -X POST http://localhost:8000/laser/connect/10.10.10.4
curl -X POST http://localhost:8000/laser/disconnect
```

## REST API

All parameter changes take effect on the next frame — nothing restarts.

| Endpoint | Description |
|----------|-------------|
| `GET /api/state` | Full current state as JSON |
| `POST /api/state` | Bulk update (send any subset of fields as JSON) |
| `POST /api/reset` | Reset all show parameters to defaults (keeps the current connection) |
| `POST /laser/shape/<shape>` | `circle`, `line`, `triangle`, `square`, `wave`, `staticwave` |
| `POST /laser/brightness/<0-1>` | Master brightness |
| `POST /laser/color/<r>/<g>/<b>` | Beam color, each channel 0..1 |
| `POST /laser/position/<x>/<y>` | Shape offset, each axis -1..1 |
| `POST /laser/rotation/speed/<v>` | Rotation speed in rotations/sec |
| `POST /move/mode/<mode>` | `none`, `circle`, `pan`, `tilt`, `eight`, `random` |
| `POST /move/speed/<v>` / `/move/size/<v>` | Movement cycle speed / amplitude |
| `POST /laser/rainbow/amount/<v>` / `/speed/<v>` | Rainbow color blend / hue cycle speed |
| `POST /blackout/<0\|1>` | Force *hardware* output dark (never affects the browser preview) |
| `POST /brightness/gate/<0\|1>` | Footswitch-style master brightness gate (affects preview + hardware): 1 = open instantly (renders the current brightness fader value), 0 = closed (fades to dark over `flash_release_ms`, or instantly if it's 0) — defaults closed until set. Never touches the `intensity` field itself |
| `POST /flash/<0\|1>` | Momentary flash: 1 = press (forces color to white + full brightness, remembering the prior values), 0 = release (always restores them instantly - `flash_release_ms` does not apply here, only to the footswitch gate above) |
| `POST /flash/release_ms/<v>` | Footswitch gate release fade time (0-2000ms), despite the name this only affects `/brightness/gate` closing, not flash's own release: `0` = instant cut, `>0` = fade brightness to 0 over this many ms instead of snapping to dark (default `200`) |
| `POST /mirror/x/<0\|1>` | Flip the output horizontally (mirror around center); 1 = mirrored, 0 = normal |
| `POST /motion/hold/<0\|1>` | Momentary freeze: 1 = press (stops movement, rotation, and the rainbow hue cycle in place, remembering the prior speeds), 0 = release (restores them) |
| `POST /rotation/reset` | Snap the rotation angle back to 0 and stop it spinning (sets `rotation_speed` to 0) |
| `POST /laser/connect/<ip>` | Connect + arm a controller at this IP |
| `POST /laser/disconnect` | Disarm and disconnect |
| `GET /api/cues` | List all populated cue slots (1-32) |
| `POST /api/cue/<n>/save` | Snapshot current show params into slot `n` |
| `POST /api/cue/<n>/recall` | Apply slot `n`'s saved show params (keeps the current connection and the global `flash_release_ms` setting - see below) |
| `POST /api/cue/<n>/clear` | Delete slot `n` |
| `POST /api/cue/<from>/move/<to>` | Move slot `from`'s cue into slot `to` (overwriting it), clearing `from`. 404 if `from` is empty or `from == to` |
| `WS /ws/points` | Live preview stream, `{"pts":[[x,y,r,g,b],...]}` at ~30fps |

`POST /api/state` accepts a JSON body with any of: `shape`, `radius`, `points`,
`rate_kpps`, `max_rate_kpps`, `intensity`, `flash_release_ms`, `r`, `g`, `b`,
`shape_scale`, `pos_x`, `pos_y`, `rotation_speed`, `mirror_x`, `move_mode`,
`move_speed`, `move_size`, `wave_frequency`, `wave_amplitude`, `wave_speed`,
`rainbow_amount`, `rainbow_speed`, `blackout`, `dot_amount`, `flicker_hz`, `ip`.

`flash_release_ms`, `intensity`, `blackout` and `max_rate_kpps` (like `ip`/the
controller connection) are *global* daemon settings, not part of a cue's show
state: saving a cue never captures them and recalling one never changes them,
no matter what they were set to when the cue was saved. `max_rate_kpps` also
caps and defaults `rate_kpps` on the fader/REST/MIDI - turning it down lowers
the scan-rate fader's usable range too.

## Multi-client sync

The frontend polls `GET /api/state` every second, so if you change settings
from another browser tab or via `curl`, all connected UIs pick up the change
automatically.

## MIDI control (optional)

Any USB MIDI controller — including an Akai APC40 mkII, like the original
BeamCommander — can drive the exact same state as the REST API and the web
UI. It's entirely optional: with nothing plugged in (or no bindings
configured), this subsystem simply does nothing.

1. Plug the controller in via USB, then start `laser_daemon` — it
   auto-detects and connects to every available MIDI source (and keeps
   scanning every few seconds for hot-plugged devices, no restart needed).
   A footswitch plugged into the APC40's 1/4" jack works too — it sends CC64
   (the standard MIDI "sustain pedal" number), bound by default to
   `brightness_gate`: a "hold to show light" master switch for both the
   preview and the real laser output — hold the pedal down to let the
   current brightness fader value through instantly, release it to fade to
   dark over `flash_release_ms` (instantly if that's 0) regardless of what
   the fader is set to; the fader itself is never touched, so whatever it
   was last set to takes over immediately the next time the pedal is
   pressed. Touching the fader directly (UI/REST/MIDI) also opens the gate
   right away, so brightness isn't stuck dark if you never use the pedal.
   Most footswitches are wired normally-closed, so they report the
   *opposite* of what you'd expect (high while up, low while pressed) — the
   shipped CC64 binding has `invert: true` set to compensate; flip it if
   your pedal is normally-open instead.
2. Bindings live in `backend/midi_map.json`, a plain JSON array of
   `{ "type": "note"|"cc", "channel": 0-15|-1, "number": 0-127, "action": "..." }`
   objects (`channel: -1` matches any channel). The shipped default is
   ported directly from the original BeamCommander's Akai APC40 mkII mapping
   (`MidiToOscMapper.h` / `midi_mapping.json`), including its response-curve
   engine (see "CC response curve" below) — so it should work out of the box
   with that exact controller. For any other controller, or if your APC40's
   firmware mode sends different numbers, **the console log is the source of
   truth** (see step 3).
3. To calibrate: press a button / turn a knob on your controller and watch
   the daemon's console output. Every message that doesn't match a binding
   is logged, e.g. `[midi] unmapped cc ch=0 num=90 val=42` — add/edit the
   matching entry in `midi_map.json` and restart the daemon.

### CC response curve

Each `cc` binding can optionally shape its raw 0-127 value before it's
applied, matching the original BeamCommander's MIDI mapper:

| Field | Default | Meaning |
|-------|---------|---------|
| `centered` | `false` | Bipolar knob: raw MIDI 63/64 is treated as the *exact* center (0), not just `val/127`. Use for knobs that should rest at a neutral value in the middle of their travel (position, rotation speed, rainbow speed). |
| `deadzone` | `0` | (centered only) snap to exactly 0 within this distance of center (0..1) — stops a physical knob's center detent from drifting. |
| `gamma` | `1.0` | Response curve exponent (symmetric around center when `centered`). `>1` = more resolution near the low end / center; `<1` = more resolution at the extremes. |
| `invert` | `false` | Reverse direction. |
| `scale`, `offset` | `1.0`, `0.0` | Applied before clamping, non-centered mode only. |
| `outMin`, `outMax` | `0.0`, `1.0` | Final mapped range for the target field. |
| `relative` | `false` | Treat raw as a two's-complement tick delta from an endless/relative encoder (e.g. the APC40's "Cue Level" knob) instead of an absolute position: `1-63` = turned up that many ticks, `65-127` = turned down `128-raw` ticks. Each tick nudges a persistent 0..1 accumulator by `step` (still passed through `gamma`/`outMin`/`outMax` afterward) rather than jumping to a position. |
| `step` | `0.01` | (relative only) how much of the 0..1 accumulator range each single tick moves. |

### Action catalog

**Continuous (`cc`, mapped into each action's own natural range via the
curve fields above):**
`r`, `g`, `b`, `intensity`, `shape_scale`, `rotation_speed`, `pos_x`, `pos_y`,
`dot_amount`, `flicker_hz`, `wave_frequency`, `wave_amplitude`, `wave_speed`,
`rainbow_amount`, `rainbow_speed`, `move_size`, `move_speed`, `rate_kpps`,
`flash_release_ms` (0-2000ms; ported from the original BeamCommander's
`flashReleaseMs` knob - the shipped binding uses `relative: true` since the
APC40's "Cue Level" knob this is mapped to is an endless encoder, not an
absolute pot, see "CC response curve" above. Despite the name, this only
controls how fast the footswitch's `brightness_gate` fades to dark on
release - it does *not* affect the `flash` action's own release, which is
always instant)

**Footswitch-style (`cc`, treated as above/below the midpoint rather than a
continuous value):**
`flash` (see below - reachable from a footswitch CC as well as note buttons),
`brightness_gate` (the CC64 footswitch default: pedal held = brightness
shows the current fader value instantly, pedal up = fades to dark over
`flash_release_ms` (instantly if 0) - affects the preview and hardware
together, and never touches the `intensity` field itself, so the fader's
last value always takes over immediately the next time the pedal is
pressed).

**Buttons (`note`, note-on presses / note-off or velocity-0 releases):**
`shape:<circle|line|triangle|square|wave|staticwave>`,
`move:<none|circle|pan|tilt|eight|random>`,
`color:<red|green|blue|white>` (plus `orange`, `yellow`, `cyan`, `magenta` if
you want to bind extra buttons to them),
`rainbow_preset_slowfull` (full rainbow blend at a slow speed, one press),
`blackout_toggle` / `blackout_hold` (dark until pressed again, vs. dark only
while held — the original APC40 mapping uses `_hold`),
`flash` (forces color to white *and* full brightness only while held - also
forces the brightness gate open, so it's always visible even if the
footswitch was never pressed; on release, color and brightness always
restore to the prior values instantly - `flash_release_ms` does not apply
here, only to the footswitch gate's own release, see above),
`motion_hold`
(freezes the current movement pattern, rotation, *and* rainbow hue cycle in
place while held, resumes all three on release), `mirror_hold` (flips the
output horizontally while held, un-flips on release),
`rotation_reset` (one-shot: snaps the rotation angle back to 0 and stops it
spinning, unlike the hold actions above this fires once on press and doesn't
do anything special on release),
`cue_save_arm` (arm save mode — the *next* `cue:<n>` button saves instead of
recalls) + `cue:<1-32>` (latching save/recall),
`cue_momentary:<1-32>` (hold to preview that cue, release to snap back to
whatever was showing before — matches the original APC40 grid buttons).

## Supported hardware

Via libera-laser: Ether Dream, Helios USB, Helios Pro (IDN), LaserCube USB,
LaserCube Network, AVB/Audio.

## Development

See [DEVELOPER.md](DEVELOPER.md) for build instructions and internals.
