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
| `POST /blackout/<0\|1>` | Force output dark |
| `POST /laser/connect/<ip>` | Connect + arm a controller at this IP |
| `POST /laser/disconnect` | Disarm and disconnect |
| `GET /api/cues` | List all populated cue slots (1-32) |
| `POST /api/cue/<n>/save` | Snapshot current show params into slot `n` |
| `POST /api/cue/<n>/recall` | Apply slot `n`'s saved show params (keeps the current connection) |
| `POST /api/cue/<n>/clear` | Delete slot `n` |
| `WS /ws/points` | Live preview stream, `{"pts":[[x,y,r,g,b],...]}` at ~30fps |

`POST /api/state` accepts a JSON body with any of: `shape`, `radius`, `points`,
`rate_kpps`, `intensity`, `r`, `g`, `b`, `shape_scale`, `pos_x`, `pos_y`,
`rotation_speed`, `move_mode`, `move_speed`, `move_size`, `wave_frequency`,
`wave_amplitude`, `wave_speed`, `rainbow_amount`, `rainbow_speed`, `blackout`,
`dot_amount`, `flicker_hz`, `ip`.

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

### Action catalog

**Continuous (`cc`, mapped into each action's own natural range via the
curve fields above):**
`r`, `g`, `b`, `intensity`, `shape_scale`, `rotation_speed`, `pos_x`, `pos_y`,
`dot_amount`, `flicker_hz`, `wave_frequency`, `wave_amplitude`, `wave_speed`,
`rainbow_amount`, `rainbow_speed`, `move_size`, `move_speed`, `rate_kpps`

**Buttons (`note`, note-on presses / note-off or velocity-0 releases):**
`shape:<circle|line|triangle|square|wave|staticwave>`,
`move:<none|circle|pan|tilt|eight|random>`,
`color:<red|green|blue|white>` (plus `orange`, `yellow`, `cyan`, `magenta` if
you want to bind extra buttons to them),
`rainbow_preset_slowfull` (full rainbow blend at a slow speed, one press),
`blackout_toggle` / `blackout_hold` (dark until pressed again, vs. dark only
while held — the original APC40 mapping uses `_hold`),
`flash` (full brightness while held, restores previous brightness on
release), `flash_white` (forces color to white *and* full brightness while
held, restores both on release), `motion_hold` (freezes the current
movement pattern in place while held, resumes on release),
`cue_save_arm` (arm save mode — the *next* `cue:<n>` button saves instead of
recalls) + `cue:<1-32>` (latching save/recall),
`cue_momentary:<1-32>` (hold to preview that cue, release to snap back to
whatever was showing before — matches the original APC40 grid buttons).

## Supported hardware

Via libera-laser: Ether Dream, Helios USB, Helios Pro (IDN), LaserCube USB,
LaserCube Network, AVB/Audio.

## Development

See [DEVELOPER.md](DEVELOPER.md) for build instructions and internals.
