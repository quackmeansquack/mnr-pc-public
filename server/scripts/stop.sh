#!/bin/bash
# Stop the private server (Bombd, PLGarage, the loops). Run as root.
pkill -f "bombd-loop.sh|backup-loop.sh"; pkill -f "bombd-app/Bombd"; pkill -f "GameServer.dll"
