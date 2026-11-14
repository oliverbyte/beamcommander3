#!/usr/bin/env bash
# Writes a ready-to-boot BeamCommander3 SD card for a Raspberry Pi 2/3/4
# (32-bit Raspberry Pi OS Lite) on macOS - see README.md in this folder.
#
#   packaging/raspberry-pi/make_sdcard.sh disk4
#
# 1. syncs the current user data (app data dir -> backend/), so the
#    card gets exactly the settings in use on this Mac,
# 2. downloads + verifies Raspberry Pi OS Lite (cached in $BC3_PI_WORK),
# 3. builds laser_daemon for armhf (build_daemon.sh) if not built yet and the
#    frontend,
# 4. writes the image to the card (asks for confirmation + sudo password),
# 5. adds the first-boot setup to the boot partition: user + password,
#    static IP (boot/network-config), and bc3/ (installed by files/install.sh).
#
# Env: PI_USER (default bc3), PI_PASSWORD (asked if unset),
#      BC3_PI_WORK (default ~/bc3-pi), REBUILD=1 forces a daemon rebuild.
set -euo pipefail
DISK="${1:?usage: make_sdcard.sh diskN   (see: diskutil list external)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${BC3_PI_WORK:-$HOME/bc3-pi}"
IMG_NAME=2026-10-06-raspios-trixie-armhf-lite
IMG_URL="https://downloads.raspberrypi.com/raspios_lite_armhf/images/raspios_lite_armhf-2026-10-06/$IMG_NAME.img.xz"
PI_USER="${PI_USER:-bc3}"

# ── Target disk: only an external, removable disk ──────────────────────────
info="$(diskutil info "$DISK")"
grep -q "Device Location: *External" <<<"$info" || { echo "$DISK is not external - abort"; exit 1; }
grep -q "Removable Media: *Removable" <<<"$info" || { echo "$DISK is not removable - abort"; exit 1; }
diskutil list "$DISK"
read -r -p "ERASE everything on $DISK? Type the disk name to confirm: " answer
[[ "$answer" == "$DISK" ]] || { echo "not confirmed - abort"; exit 1; }
if [[ -z "${PI_PASSWORD:-}" ]]; then
  read -r -s -p "Password for Pi user '$PI_USER': " PI_PASSWORD; echo
fi
[[ -n "$PI_PASSWORD" ]] || { echo "empty password - abort"; exit 1; }

# ── Current settings into the repo ─────────────────────────────────────────
bash "$REPO/packaging/sync_default_data.sh"

# ── Image ──────────────────────────────────────────────────────────────────
mkdir -p "$WORK/image"
IMG="$WORK/image/$IMG_NAME.img"
if [[ ! -f "$IMG" ]]; then
  (cd "$WORK/image"
   curl -fL# -O "$IMG_URL" -O "$IMG_URL.sha256"
   shasum -a 256 -c "$IMG_NAME.img.xz.sha256"
   xz -dk -T0 "$IMG_NAME.img.xz")
fi

# ── Daemon + frontend ──────────────────────────────────────────────────────
[[ -x "$WORK/out/laser_daemon" && -z "${REBUILD:-}" ]] || bash "$HERE/build_daemon.sh"
(cd "$REPO/frontend" && npm run build --silent)

# ── Boot partition payload ─────────────────────────────────────────────────
STAGE="$WORK/boot"
rm -rf "$STAGE"; mkdir -p "$STAGE/bc3/data"
touch "$STAGE/ssh"
echo "$PI_USER:$(openssl passwd -6 "$PI_PASSWORD")" > "$STAGE/userconf.txt"
cp "$HERE/boot/user-data" "$HERE/boot/network-config" "$STAGE/"
cp "$HERE"/files/* "$STAGE/bc3/"
cp "$WORK/out/laser_daemon" "$STAGE/bc3/"
cp -R "$REPO/frontend/dist" "$STAGE/bc3/frontend_dist"
# First-run seeds next to the binary (like the desktop releases) ...
cp "$REPO"/backend/{cues,magic_touch,midi_map,lasers,settings}.json "$STAGE/bc3/"
# ... and the complete data set for the Pi's data directory.
cp "$REPO"/backend/{cues,magic_touch,lasers,settings}.json "$STAGE/bc3/data/"
chmod +x "$STAGE"/bc3/*.sh "$STAGE/bc3/laser_daemon"

# ── Write the card ─────────────────────────────────────────────────────────
diskutil unmountDisk "/dev/$DISK"
echo "[pi] writing image to /dev/r$DISK (a few minutes)..."
sudo dd if="$IMG" of="/dev/r$DISK" bs=4m status=progress
sync
diskutil mountDisk "/dev/$DISK" || true
BOOT=/Volumes/bootfs
for _ in $(seq 1 20); do [[ -d "$BOOT" ]] && break; sleep 1; done
[[ -d "$BOOT" ]] || { echo "boot partition did not mount - abort"; exit 1; }
cp "$STAGE"/{ssh,userconf.txt,user-data,network-config} "$BOOT/"
cp -R "$STAGE/bc3" "$BOOT/"
find "$BOOT" -name '._*' -delete 2>/dev/null || true
sync
diskutil eject "/dev/$DISK"
echo "[pi] done - put the card into the Pi and power it up."
echo "     UI: http://10.10.10.40:8000   SSH: ssh $PI_USER@10.10.10.40"
