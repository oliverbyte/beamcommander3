# BeamCommander3 on a Raspberry Pi (standalone)

Turns a Raspberry Pi into a standalone BeamCommander3 box: it boots, starts
the laser daemon on its own and serves the UI to any browser on the laser
network. Made for a Raspberry Pi 2 Model B v1.1 (32-bit only); the same card
also boots on a Pi 3/4.

| | |
|---|---|
| OS | Raspberry Pi OS Lite (Trixie, 32-bit armhf), 2026-10-06 |
| Network | `eth0` static `10.10.10.40/24`, no gateway (`boot/network-config`) |
| Hostname | `beamcommander` (`beamcommander.local` via mDNS) |
| UI | http://10.10.10.40:8000 |
| Login | SSH/console, user `bc3` (password chosen when writing the card), sudo |
| Daemon | `/opt/beamcommander3/laser_daemon`, systemd `beamcommander3.service` |
| Data | `/var/lib/beamcommander3/BeamCommander3/` (on the SD card, persistent) |

## Write a card (macOS)

```sh
diskutil list external                       # find the SD card, e.g. disk4
packaging/raspberry-pi/make_sdcard.sh disk4
```

The script asks you to type the disk name to confirm the erase, asks for the
Pi user's password, then:

1. runs `packaging/sync_default_data.sh`, so the card gets the settings
   currently in use on this Mac: cues and Magic Touch layout (`backend/`) plus
   this rig's lasers and Beam settings (`rig/lasers.json`, `rig/settings.json`),
2. downloads and verifies the Raspberry Pi OS image (cached in `~/bc3-pi`),
3. builds `laser_daemon` for armhf in Docker (`build_daemon.sh`, ~10 min
   the first time; `REBUILD=1` forces a rebuild after backend changes) and the
   frontend,
4. writes the image (`sudo dd`) and copies the first-boot setup onto the
   boot partition.

Needs Docker Desktop (it emulates `linux/arm/v7`), Node.js, `xz`.

## First boot

The first boot takes a few minutes. cloud-init sets the hostname and static
IP, `userconf.txt` creates the user, and `files/install.sh` (run from
`/boot/firmware/bc3/`, log in `bc3/install.log` on the boot partition)
installs the daemon, copies the data files into the data directory and
enables the services. Afterwards cloud-init is switched off, so later boots
are quicker.

## Running

- `beamcommander3.service` starts at boot and restarts 2 s after any crash,
  without ever giving up (`Restart=always`, `StartLimitIntervalSec=0`).
- `beamcommander3-health.timer` checks every 30 s that the daemon answers
  HTTP and restarts it if it hangs.
- The daemon always starts blacked out, even with lasers armed.
- Every change made in the UI (cues, lasers, zones, masks, Magic Touch
  layout, Beam settings) is saved to the data directory on the SD card. The
  current look itself (faders on screen) is not saved, same as on the desktop.
- MIDI is not available on the Pi (the daemon's MIDI support is macOS only).

```sh
ssh bc3@10.10.10.40
systemctl status beamcommander3
journalctl -u beamcommander3 -f
```

## Update the app on a running Pi

Copy a new build to the Pi and re-run the installer. Existing data files are
never overwritten:

```sh
packaging/raspberry-pi/build_daemon.sh
scp ~/bc3-pi/out/laser_daemon bc3@10.10.10.40:/tmp/
ssh bc3@10.10.10.40 'sudo cp /tmp/laser_daemon /boot/firmware/bc3/ && sudo bash /boot/firmware/bc3/install.sh'
```

(For a new frontend, copy `frontend/dist/` to `/boot/firmware/bc3/frontend_dist/`
the same way.)
