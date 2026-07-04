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

## Supported hardware

Via libera-laser: Ether Dream, Helios USB, Helios Pro (IDN), LaserCube USB,
LaserCube Network, AVB/Audio.

## Development

See [DEVELOPER.md](DEVELOPER.md) for build instructions and internals.
