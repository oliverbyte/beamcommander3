#!/bin/bash
# Installs BeamCommander3 from the boot partition (run once by cloud-init on
# first boot; safe to run again: sudo bash /boot/firmware/bc3/install.sh).
# Existing app data in /var/lib/beamcommander3 is never overwritten.
set -euxo pipefail
SRC=/boot/firmware/bc3
APP=/opt/beamcommander3
DATA=/var/lib/beamcommander3/BeamCommander3

id beamcommander >/dev/null 2>&1 || \
  useradd --system --home-dir /var/lib/beamcommander3 --shell /usr/sbin/nologin --groups plugdev beamcommander

systemctl stop beamcommander3 2>/dev/null || true
rm -rf "$APP"; install -d "$APP"
install -m 755 "$SRC/laser_daemon" "$SRC/beamcommander3-health.sh" "$APP/"
cp -r "$SRC/frontend_dist" "$APP/"
# Seeds, copied into the data dir by the daemon if missing there.
cp "$SRC/cues.json" "$SRC/magic_touch.json" "$SRC/midi_map.json" "$APP/"

install -d "$DATA"
for f in "$SRC"/data/*.json; do
  [ -e "$DATA/$(basename "$f")" ] || cp "$f" "$DATA/"
done
chown -R beamcommander:beamcommander /var/lib/beamcommander3

install -m 644 "$SRC"/beamcommander3.service "$SRC"/beamcommander3-health.service \
  "$SRC"/beamcommander3-health.timer /etc/systemd/system/
install -m 644 "$SRC/60-beamcommander3-dacs.rules" /etc/udev/rules.d/
udevadm control --reload || true

systemctl daemon-reload
systemctl enable ssh beamcommander3 beamcommander3-health.timer
systemctl restart beamcommander3
systemctl start ssh beamcommander3-health.timer

# Setup is done - skip cloud-init on later boots (faster start).
touch /etc/cloud/cloud-init.disabled
echo "BeamCommander3 installed."
