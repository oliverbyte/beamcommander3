# BeamCommander3

Real-time laser show control. A single C++ backend (`laser_daemon`) drives real
laser hardware via [libera-laser](https://github.com/sebleedelisle/libera-laser)
and serves a Vue 3 web UI with a live Three.js preview — the preview always
shows exactly what the backend is currently rendering, whether or not a laser
is connected.

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
| `POST /flash/release_ms/<v>` | Footswitch gate release fade time (0-2000ms), despite the name this only affects `/brightness/gate` closing, not flash's own release: `0` = instant cut (default), `>0` = fade brightness to 0 over this many ms instead of snapping to dark |
| `POST /mirror/x/<0\|1>` | Flip the output horizontally (mirror around center); 1 = mirrored, 0 = normal |
| `POST /motion/hold/<0\|1>` | Momentary freeze: 1 = press (stops movement, rotation, and the rainbow hue cycle in place, remembering the prior speeds), 0 = release (restores them) |
| `POST /rotation/reset` | Snap the rotation angle back to 0 and stop it spinning (sets `rotation_speed` to 0) |
| `POST /laser/connect/<ip>` | Connect + arm a controller at this IP |
| `POST /laser/disconnect` | Disarm and disconnect |
| `GET /api/cues` | List all populated cue slots (1-32) |
| `POST /api/cue/<n>/save` | Snapshot current show params into slot `n` |
| `POST /api/cue/<n>/recall` | Apply slot `n`'s saved show params (keeps the current connection) |
| `POST /api/cue/<n>/clear` | Delete slot `n` |
| `POST /api/cue/<from>/move/<to>` | Move slot `from`'s cue into slot `to` (overwriting it), clearing `from`. 404 if `from` is empty or `from == to` |
| `WS /ws/points` | Live preview stream, `{"pts":[[x,y,r,g,b],...]}` at ~30fps |

`POST /api/state` accepts a JSON body with any of: `shape`, `radius`, `points`,
`rate_kpps`, `intensity`, `flash_release_ms`, `r`, `g`, `b`, `shape_scale`,
`pos_x`, `pos_y`, `rotation_speed`, `mirror_x`, `move_mode`, `move_speed`,
`move_size`, `wave_frequency`, `wave_amplitude`, `wave_speed`, `rainbow_amount`,
`rainbow_speed`, `blackout`, `dot_amount`, `flicker_hz`, `ip`.

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
