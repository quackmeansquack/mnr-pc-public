#!/bin/bash
# One-time setup of the server (run as root after building PLGarage and Bombd: bash configure.sh <public IP>).
# Makes the PLGarage database and its account, random keys, a self-signed certificate for Bombd, and both config
# files. <public IP> is the address players reach the server on (Bombd hands it to the games). Run it again with the
# new address if that changes; the keys are kept. Secrets stay in /home/mnr-server/srv/secrets.env (never commit it).
set -e
IP="${1:?usage: configure.sh <public IP of the server>}"
U=mnr-server
S=/home/$U/srv
SEC=$S/secrets.env
[ -f "$SEC" ] || {
    echo "DBPW=$(openssl rand -hex 16)" > "$SEC"
    echo "COMMKEY=$(openssl rand -hex 24)" >> "$SEC"
    echo "JWTKEY=$(openssl rand -hex 32)" >> "$SEC"
    echo "PFXPW=$(openssl rand -hex 12)" >> "$SEC"
    chown $U:$U "$SEC"; chmod 600 "$SEC"
}
. "$SEC"

service mariadb status > /dev/null 2>&1 || service mariadb start
mariadb -e "CREATE DATABASE IF NOT EXISTS PLGarage; CREATE USER IF NOT EXISTS 'plgarage'@'127.0.0.1' IDENTIFIED BY '$DBPW'; ALTER USER 'plgarage'@'127.0.0.1' IDENTIFIED BY '$DBPW'; GRANT ALL PRIVILEGES ON PLGarage.* TO 'plgarage'@'127.0.0.1'; FLUSH PRIVILEGES;"

# PLGarage reads ./config.json from its working directory
cat > $S/plgarage-app/config.json <<EOF
{
  "ExternalURL": "auto:10050",
  "MysqlConnectionString": "server=127.0.0.1;uid=plgarage;pwd=$DBPW;database=PLGarage",
  "InstanceName": "MNR PC",
  "ServerCommunicationKey": "$COMMKEY",
  "JWTSigningKey": "$JWTKEY",
  "BlockLBPK": true
}
EOF

# Bombd: certificate (2048-bit RSA, SHA256, as its README asks) and bombd.json, run from bombd-run with its Data folder
mkdir -p $S/bombd-run
[ -d $S/bombd-run/Data ] || cp -r $S/bombd-app/Data $S/bombd-run/
if [ ! -f $S/bombd-run/bombd.pfx ]; then
    openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes -subj "/CN=mnr-pc-server" \
        -keyout $S/bombd-run/key.pem -out $S/bombd-run/cert.pem 2>/dev/null
    openssl pkcs12 -export -out $S/bombd-run/bombd.pfx -inkey $S/bombd-run/key.pem -in $S/bombd-run/cert.pem -passout pass:$PFXPW
    rm -f $S/bombd-run/key.pem
fi
cat > $S/bombd-run/bombd.json <<EOF
{
  "ListenIP": "0.0.0.0",
  "ExternalIP": "$IP",
  "ApiURL": "http://127.0.0.1:10050",
  "PfxCertificate": "$S/bombd-run/bombd.pfx",
  "PfxKey": "$PFXPW",
  "TickRate": 15,
  "AllowKarting": false,
  "AllowModNation": true,
  "ServerCommunicationKey": "$COMMKEY"
}
EOF
chown -R $U:$U $S/plgarage-app/config.json $S/bombd-run
chmod 600 $S/plgarage-app/config.json $S/bombd-run/bombd.json
echo "configured for $IP"
