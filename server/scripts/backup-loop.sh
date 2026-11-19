#!/bin/bash
# Runs backup.sh every night at 4:00 while the server is up (start.sh starts this; WSL1 runs no cron). As root.
HERE=$(dirname "$(readlink -f "$0")")
while true; do
    now=$(date +%s); next=$(date -d "tomorrow 04:00" +%s)
    [ "$(date +%H)" -lt 4 ] && next=$(date -d "today 04:00" +%s)
    sleep $((next - now))
    bash "$HERE/backup.sh"
done
