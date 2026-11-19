#!/bin/bash
# The server dashboard (dashboard/dashboard.py), restarted if it exits. start.sh starts this as mnr-server.
# Open http://127.0.0.1:10090 on this PC.
D=/home/mnr-server/srv/dashboard
cd "$D"
[ -f "$D/dashboard.env" ] && set -a && . "$D/dashboard.env" && set +a   # e.g. MNR_SERVER_NAME=my.server.net
while true; do
    python3 "$D/dashboard.py" >> "$D/dashboard.log" 2>&1
    echo "[restart] dashboard exited ($?), restarting in 5 s" >> "$D/dashboard.log"
    sleep 5
done
