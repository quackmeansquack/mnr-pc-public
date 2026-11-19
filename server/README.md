# Running your own server

Everything here is for people who host an online server for the PC version (the address players pick under
**Settings > Online** in the launcher). If you only play, you don't need this folder.

**The launcher doesn't use this folder.** It only downloads releases (`runtime.zip` and the launcher itself), never
these files, and the publish scripts leave `server/` alone. Nothing here holds game files: the two lists the server
needs from the game are copied from your own copy by `patch_plgarage.py`.

## What the server is

Two open-source programs, plus the patch below:

| Program | What it does | Source | Tested at |
| --- | --- | --- | --- |
| PLGarage | The web side: sign-in, profiles, creations, leaderboards (TCP 10050) | [jackcaver/PLGarage](https://github.com/jackcaver/PLGarage) | `68a228b` |
| Bombd | Matchmaking and the race rooms (TCP 10412, 10501, 10505, 10510, 10513, 10514, 13452; UDP 50002) | [ennuo/Bombd](https://github.com/ennuo/Bombd) | `d7ab87c` |

Bombd is used unchanged. PLGarage gets one added file, and none of its own code is changed.

## The patch: `plgarage/MnrPcController.cs`

`patch_plgarage.py` copies it into PLGarage, with two lists taken from your game. It adds:

- **Creations from MNR Revival.** A creation downloaded from the [MNR Revival](https://mnrrevival.derole.net) site
  keeps its Revival id. A stock PLGarage doesn't know that id and tells the game the creation is banned, so the game
  won't use it. With the patch, the server copies the creation from Revival under the same id:
  - when the launcher's creation browser installs one (`POST /mnrpc/import?id=<id>`; the launcher checks
    `GET /mnrpc/caps` to see whether a server can do this);
  - on the fly, when a game asks the server for a Revival creation it doesn't have yet (someone wears a Revival
    character in a lobby, a room picks a Revival track).

  The server fetches the creation itself; players only name an id, so nothing a player uploads is stored this way.
  A remixed creation's original comes along, up to 3 generations. Each creator gets an account named after their
  Revival username, without a sign-in attached, which becomes theirs if they ever sign in with that name. Ids
  10000 to 999999 are Revival's, so the server's own new creations must start at 1000000 (step 8 below). Each
  address can import up to 200 creations an hour.
- **Creation tags and the chat filter.** The game downloads its keyword tags and chat filter from the server; stock
  PLGarage doesn't answer those (`tags.xml`, `profanity_filters.xml`), so the tag lists stay empty and the game logs
  "failed to download profanity filter". The patch answers with the game's own lists (`CREATIONKEYWORDTAGS.XML` and
  `PROFANITYFILTER.XML` from the game's DATA folder).
- **A status page** at `http://<server>:10050/mnrpc/status`, answered only on the server itself and your home
  network: who is online, creation counts, recent imports, accounts.
- **Repair** (`POST /mnrpc/repair`, from the server itself only): fills in the originals of creations imported
  before originals came along.
- **Logging** of friends' creation and leaderboard requests, to look into an empty Download list.

The Revival address can be changed with the environment variable `MNRPC_REVIVAL_URL`.

## Setting it up

Tested on Ubuntu 26.04 under WSL1 on Windows 10 (the PC that hosts runs it), and on Ubuntu 24.04 on a Google Cloud
e2-micro. The scripts assume a Linux user `mnr-server` with everything in `/home/mnr-server/srv`.

1. **Linux.** On Windows, turn on WSL and install Ubuntu (in an admin PowerShell:
   `wsl --install -d Ubuntu`, then restart). WSL1 shares Windows' network, which keeps port forwarding simple:
   `wsl --set-default-version 1` before installing, or `wsl --set-version Ubuntu 1` after.
2. **Packages and the user** (as root, `sudo -i`):
   ```bash
   apt update
   apt install -y git python3 curl build-essential perl mariadb-server dotnet-sdk-8.0
   useradd -m -s /bin/bash mnr-server
   ```
   A newer .NET SDK works too (the scripts set `DOTNET_ROLL_FORWARD=Major`).
3. **Sources** (as `mnr-server`: `su - mnr-server`):
   ```bash
   mkdir -p ~/srv && cd ~/srv
   git clone https://github.com/jackcaver/PLGarage.git
   git clone https://github.com/ennuo/Bombd.git
   ```
   Then copy this repository's `server` folder to `~/srv/mnr` (for example from
   `/mnt/c/Users/<you>/Downloads/...`), and its scripts to `~/srv/scripts`: `mkdir -p ~/srv/scripts && cp ~/srv/mnr/scripts/*.sh ~/srv/scripts/`.
4. **OpenSSL for Bombd:** `bash ~/srv/scripts/build_openssl.sh` (into `~/srv/openssl`).
5. **The patch, then build both programs:**
   ```bash
   python3 ~/srv/mnr/patch_plgarage.py /mnt/c/Users/<you>/Games/MNR/extracted/PS3_GAME/USRDIR/DATA
   cd ~/srv/PLGarage/GameServer && dotnet publish -c Release -o ~/srv/plgarage-app
   cd ~/srv/Bombd && dotnet publish Bombd -c Release -o ~/srv/bombd-app
   ```
   The DATA folder is in your own game copy, where the launcher set the game up.
6. **Configure** (as root): `bash /home/mnr-server/srv/scripts/configure.sh <your public IP>`. It makes the
   database and its account, random keys in `~/srv/secrets.env`, Bombd's certificate, and both config files. Run it
   again with the new address if your public IP changes.
7. **Start** (as root): `bash /home/mnr-server/srv/scripts/start.sh`. It starts MariaDB, PLGarage, Bombd (restarted if
   it stops), the nightly backups and the dashboard. `stop.sh` stops them. On Windows, to start it at sign-in, put a
   file `mnr-server.vbs` in `shell:startup` with:
   ```vb
   CreateObject("WScript.Shell").Run "wsl.exe -d Ubuntu -u root -- bash /home/mnr-server/srv/scripts/start.sh", 0, False
   ```
8. **Once, after the first start** (PLGarage has made its tables), give the server's own creations ids above
   Revival's: `mariadb PLGarage -e "ALTER TABLE PlayerCreations AUTO_INCREMENT = 1000000"`.
9. **Open the ports.** Forward the ports in the table above from your router to this PC (and allow them in Windows
   Firewall), or open them in your cloud firewall. A DNS name pointing at your public IP is easier to hand out than
   the IP itself.
10. **Players** enter that name or IP under **Settings > Online > Other…** in the launcher.

## Day to day

- **Logs:** `~/srv/plgarage.log` (emptied each time PLGarage starts) and `~/srv/bombd.log`.
- **Dashboard** (`dashboard/`): `http://127.0.0.1:10090` on the server shows its health, players, races, creations
  and both logs, live. It only reads (the logs, the backups and the database), and listens on the server itself only.
  Copy `dashboard/` to `~/srv/dashboard`; `start.sh` starts it. Put `MNR_SERVER_NAME=<your address>` in
  `~/srv/dashboard/dashboard.env` to see where that address points. It keeps a dated copy of PLGarage's log in
  `dashboard/plgarage-history.log`, since PLGarage empties its own on every start.
- **Backups:** every night at 4:00 into `~/srv/backups/<date>` (the database and the uploaded creations), newest 7
  kept. Run `scripts/backup.sh` (as root) for one now.
- **Updating PLGarage:** `git pull` in `~/srv/PLGarage`, run step 5 again, then `stop.sh` and `start.sh`.

## Never share

`~/srv/secrets.env`, `~/srv/plgarage-app/config.json`, `~/srv/bombd-run/bombd.json` and `bombd.pfx` hold your keys
and passwords. Don't commit them or post them with logs.
