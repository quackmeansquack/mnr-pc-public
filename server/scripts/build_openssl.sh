#!/bin/bash
# OpenSSL with SSL3 and the old ciphers (RC4-MD5) the game's network code still uses, built for Bombd as Bombd's own
# Dockerfile does, into ~/srv/openssl. Run as the server user (mnr-server): bash build_openssl.sh [version]
# Takes a few minutes (hours on a 1-core cloud VM). The tested version is 3.5.5.
set -e
V="${1:-3.5.5}"
cd ~/srv
L=~/srv/openssl-build.log
[ -f openssl-$V.tar.gz ] || curl -fL -o openssl-$V.tar.gz "https://github.com/openssl/openssl/releases/download/openssl-$V/openssl-$V.tar.gz"
rm -rf openssl-$V && tar -xf openssl-$V.tar.gz
cd openssl-$V
./Configure enable-weak-ssl-ciphers enable-ssl3 enable-ssl3-method --prefix="$HOME/srv/openssl" --libdir=lib > "$L" 2>&1
make -j"$(nproc)" >> "$L" 2>&1
make install_sw >> "$L" 2>&1
LD_LIBRARY_PATH="$HOME/srv/openssl/lib" "$HOME/srv/openssl/bin/openssl" version
