# BeamCommander3

Laser show control software using [libera-laser](https://github.com/sebleedelisle/libera-laser) for hardware output.

## Components

| Component | Description |
|-----------|-------------|
| `daemon/circle_daemon` | C++ binary — streams a configurable circle to any libera-supported laser controller (Ether Dream, Helios, LaserCube, …) |
| `frontend/` | Vue 3 + Three.js web preview (simulation only) |
| `backend/` | Python FastAPI server for the web UI (simulation only, no hardware) |

## Quick start — hardware output

```sh
# Stream a green circle to the Ether Dream at 10.10.10.4
./daemon/circle_daemon --ip 10.10.10.4

# Adjust parameters
./daemon/circle_daemon --ip 10.10.10.4 --radius 0.5 --rate 20 --intensity 0.3

# Show all options
./daemon/circle_daemon -h
```

## Options (`circle_daemon`)

| Flag | Default | Description |
|------|---------|-------------|
| `--ip <addr>` | (any) | Only connect to the controller at this IP |
| `--radius <f>` | 0.7 | Circle radius in normalised coords 0..1 |
| `--points <n>` | 300 | Points per circle frame |
| `--rate <kpps>` | 30 | Scan rate in kilo-points-per-second |
| `--red <f>` | 0.0 | Red channel 0..1 |
| `--green <f>` | 1.0 | Green channel 0..1 |
| `--blue <f>` | 0.314 | Blue channel 0..1 |
| `--intensity <f>` | 1.0 | Master intensity multiplier 0..1 |
| `-h`, `--help` | | Show help and exit |

## Web preview (simulation)

```sh
# Terminal 1 — backend
cd backend && .venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000

# Terminal 2 — frontend
cd frontend && npm run dev -- --port 5173 < /dev/null
```

Then open http://localhost:5173

## Supported hardware

Via libera-laser: Ether Dream, Helios USB, Helios Pro (IDN), LaserCube USB, LaserCube Network, AVB/Audio.
