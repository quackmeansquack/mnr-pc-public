#!/bin/bash
# Bombd, restarted if it exits (started by start.sh as mnr-server)
S=/home/mnr-server/srv
cd $S/bombd-run
export LD_LIBRARY_PATH=$S/openssl/lib OPENSSL_CONF=$S/Bombd/docker-openssl.cnf DOTNET_ROLL_FORWARD=Major DOTNET_EnableWriteXorExecute=0
while true; do
    $S/bombd-app/Bombd >> $S/bombd.log 2>&1
    echo "[restart] Bombd exited ($?), restarting in 3 s" >> $S/bombd.log
    sleep 3
done
