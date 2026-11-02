# ModNation Racers PC (public edition)

A native Windows version of ModNation Racers (PS3, 1.11) made from your own copy of the game.

This repository holds no game files and no game code. The launcher asks for:
1. your **ModNation Racers disc** (US, BCUS98167): a decrypted disc image (.iso) or the game folder from your own dump;
2. the **1.11 update** package (it can download it from PlayStation's update server);
3. optionally a **PS3 system update file** (PS3UPDAT.PUP) for voice chat, if your disc copy doesn't carry one.

It then translates the game's own executable to C and builds the PC version on your PC with the free llvm-mingw
compiler (downloaded once, about 200 MB): about 10 to 30 minutes, once. About 20 GB of free space is needed.

Download MNRLauncher.exe from the latest release and run it.

- `launcher/MNRLauncher`: the launcher (C#, WPF); public edition: `dotnet publish -p:Edition=Public`
- `re/recomp`: the PC runtime (C) and the recompiler (`ppu2c.py`, `spu2c.py`, `build.py`)
- `re/functions_v111.tsv`: the game executable's function list; `re/mnr_race.profdata`: the build's play profile
- `patches/patches_v111.txt`: data patches
