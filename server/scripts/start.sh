#!/bin/bash
# Start the private server inside WSL: MariaDB, PLGarage (web API, TCP 10050) and Bombd (matchmaking, TCP 10412-13452
# and UDP 50002; restarted if it exits). Logs go to ~/srv/plgarage.log and ~/srv/bombd.log. Run as root (it starts MariaDB).
S=/home/mnr-server/srv
HERE=$(dirname "$(readlink -f "$0")")
service mariadb status > /dev/null 2>&1 || service mariadb start
pgrep -f "GameServer.dll" > /dev/null || su mnr-server -s /bin/bash -c "cd $S/plgarage-app && DOTNET_ROLL_FORWARD=Major DOTNET_EnableWriteXorExecute=0 nohup dotnet GameServer.dll > $S/plgarage.log 2>&1 &"
for i in $(seq 100); do (echo > /dev/tcp/127.0.0.1/10050) 2> /dev/null && break; sleep 3; done   # Bombd connects to PLGarage at start (ss doesn't work on WSL1)
pgrep -f "bombd-loop.sh" > /dev/null || su mnr-server -s /bin/bash -c "nohup bash $HERE/bombd-loop.sh > /dev/null 2>&1 &"
pgrep -f "backup-loop.sh" > /dev/null || nohup bash $HERE/backup-loop.sh > /dev/null 2>&1 &   # nightly backups at 4:00 (backup.sh)
pgrep -f "scripts/dashboard.sh" > /dev/null || su mnr-server -s /bin/bash -c "nohup bash $HERE/dashboard.sh > /dev/null 2>&1 &"   # the web dashboard, http://127.0.0.1:10090 (dashboard/)
sleep 6
pgrep -fa "GameServer.dll|bombd-app/Bombd" | cut -c1-80
