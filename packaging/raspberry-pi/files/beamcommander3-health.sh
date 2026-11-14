#!/bin/sh
# Restarts the laser daemon if it is running but no longer answers HTTP
# (hung, not crashed - crashes are handled by Restart=always).
systemctl is-active -q beamcommander3 || exit 0
# Give a freshly (re)started daemon time to come up.
since=$(systemctl show -p ActiveEnterTimestampMonotonic --value beamcommander3)
now=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)
[ $((now - since / 1000000)) -lt 60 ] && exit 0
for try in 1 2 3; do
  curl -s -o /dev/null --max-time 10 http://127.0.0.1:8000/api/state && exit 0
  sleep 5
done
echo "laser daemon not responding - restarting"
systemctl restart beamcommander3
