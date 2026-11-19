"""Adds the MNR PC patch to a PLGarage source tree. Run it inside Linux/WSL, then rebuild PLGarage:

    python3 patch_plgarage.py <your game's DATA folder> [PLGarage source folder]

<your game's DATA folder> is DATA in your own copy of the game, where the launcher put it, for example
/mnt/c/Users/<you>/Games/MNR/extracted/PS3_GAME/USRDIR/DATA (the C: drive is /mnt/c inside WSL).
The PLGarage source folder defaults to ~/srv/PLGarage.

Stock PLGarage (github.com/jackcaver/PLGarage) gets new files only; nothing of its own is changed:
  GameServer/Controllers/MnrPc/MnrPcController.cs   (plgarage/MnrPcController.cs here) see README.md
  GameServer/GameResources/mnrpc.tags.xml           the game's creation keyword tags (DATA/CREATIONKEYWORDTAGS.XML)
  GameServer/GameResources/mnrpc.profanity.xml      the game's chat filter (DATA/PROFANITYFILTER.XML)
The two XML files come from your own game copy: this repository holds no game files.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def put(src, dst):
    data = open(src, 'rb').read()
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst) and open(dst, 'rb').read() == data:
        print('already there', dst)
    else:
        open(dst, 'wb').write(data)
        print('wrote', dst)


def find(folder, name):
    """The file in folder whatever its case (the disc's names are upper case, a copy's may not be)."""
    for f in os.listdir(folder):
        if f.lower() == name.lower():
            return os.path.join(folder, f)
    sys.exit(f'{name} is not in {folder}: is that the game\'s DATA folder (PS3_GAME/USRDIR/DATA)?')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/srv/PLGarage')
    game = os.path.join(src, 'GameServer')
    if not os.path.isdir(data):
        sys.exit(f'No folder {data}')
    if not os.path.isfile(os.path.join(game, 'GameServer.csproj')):
        sys.exit(f'{src} is not a PLGarage source folder (no GameServer/GameServer.csproj)')
    put(os.path.join(HERE, 'plgarage', 'MnrPcController.cs'), os.path.join(game, 'Controllers', 'MnrPc', 'MnrPcController.cs'))
    put(find(data, 'CREATIONKEYWORDTAGS.XML'), os.path.join(game, 'GameResources', 'mnrpc.tags.xml'))
    put(find(data, 'PROFANITYFILTER.XML'), os.path.join(game, 'GameResources', 'mnrpc.profanity.xml'))
    print('Done. Now rebuild PLGarage (see README.md, step 5).')


if __name__ == '__main__':
    main()
