"""What the public repository (public edition) gets: the PC runtime's source, the recompiler, the function list, the play
profile and the data patches - never game files, the built game, firmware, saves, keys files of the private build or the
server. Used by publish_public.py: runtime_files() for the release's runtime pack, source_files() for the repository.
    python -I launcher\\public_files.py <dest>   copies the runtime files into <dest> as an install would have them
"""
import glob, os, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # MNR

# the runtime pack: what the launcher downloads and builds the game from (paths relative to MNR, as installed)
def runtime_files():
    rec = os.path.join(ROOT, 're', 'recomp')
    out = []
    for pat in ('*.c', '*.h', '*.py'):
        out += [os.path.relpath(p, ROOT).replace('\\', '/') for p in sorted(glob.glob(os.path.join(rec, pat)))]
    out += ['re/recomp/hooks_v111.txt', 're/functions_v111.tsv', 're/mnr_race.profdata', 'patches/patches_v111.txt']
    for p in out:
        if not os.path.isfile(os.path.join(ROOT, p)):
            sys.exit(f'Missing: {p}')
    return out

# the repository's source tree adds the launcher's source and these scripts (not the bin/obj build folders)
def source_files():
    out = list(runtime_files())
    src = os.path.join(ROOT, 'launcher', 'MNRLauncher')
    for d, dirs, fs in os.walk(src):
        dirs[:] = sorted(x for x in dirs if x not in ('bin', 'obj', '.vs'))
        for f in sorted(fs):
            if f.endswith(('.user', '.suo')):
                continue
            out.append(os.path.relpath(os.path.join(d, f), ROOT).replace('\\', '/'))
    out += ['launcher/public_files.py', 'launcher/publish_public.py', 'launcher/publish_release.py']   # (publish_public imports publish_release's upload code)
    return out


if __name__ == '__main__':
    dest = sys.argv[1]
    for rel in runtime_files():
        to = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, rel), to)
    print(f'{len(runtime_files())} runtime files copied to {dest}')
