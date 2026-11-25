# ModNation Racers PC (public edition)

A native Windows version of ModNation Racers (PS3, 1.11) made from your own copy of the game.

This repository holds no game files and no game code. The launcher asks for:
1. your **ModNation Racers disc** (US, BCUS98167): a decrypted disc image (.iso) or the game folder from your own dump;
2. your **1.11 update** package (UP9000-BCUS98167_00-MODNATIONKART002-A0111-V0200-PE.pkg);
3. optionally a **PS3 system update file** (PS3UPDAT.PUP) for voice chat, if your disc copy doesn't carry one.

It then translates the game's own executable to C and builds the PC version on your PC with the free llvm-mingw
compiler (downloaded once, about 200 MB): about 10 to 30 minutes, once. About 20 GB of free space is needed.

Download MNRLauncher.exe from the latest release and run it.

**Slow PC?** In the launcher, open Settings > Advanced and add a line to "Extra environment switches":
`MNR_LOW_DETAIL=1` draws single player with the game's own two-player split-screen detail (no shadows, grass or
motion blur, shorter draw distances), `MNR_LOW_DETAIL=2` with its three-player detail, lower still. The game needs
a reasonably fast processor either way: most of its work is the PS3's own processors' work, now done by your CPU.

- `launcher/MNRLauncher`: the launcher (C#, WPF); public edition: `dotnet publish -p:Edition=Public`
- `re/recomp`: the PC runtime (C) and the recompiler (`ppu2c.py`, `spu2c.py`, `build.py`)
- `re/functions_v111.tsv`: the game executable's function list; `re/mnr_race.profdata`: the build's play profile
- `patches/patches_v111.txt`: data patches
- `server/`: for people who host an online server: the PLGarage patch and how to set the server up
  ([server/README.md](server/README.md)). The launcher never downloads or changes these files.
