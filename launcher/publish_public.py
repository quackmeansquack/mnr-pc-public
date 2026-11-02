"""Publish the public edition to its repository (default quackmeansquack/mnr-pc-public):
    python -I launcher\\publish_public.py [--notes "what changed"] [--repo owner/name] [--dry-run] [--no-source] [--keep-launcher]
1. The repository's source (public_files.source_files(): the PC runtime's source, the recompiler, the function list,
   the play profile, the data patches and the launcher's source) is committed and pushed (a clone kept in
   launcher\\data\\public_repo). --no-source skips this.
2. A release: runtime.zip (public_files.runtime_files(), the files the launcher installs and builds the game from),
   the public-edition launcher (MNRLauncher.exe, built with -p:Edition=Public) and manifest.json, in the same format as
   the private releases, so the launcher's updater reads it unchanged.
Nothing else ever goes up: no game files, no built game, no firmware, no saves. The repository is created private
when it doesn't exist; make it public on GitHub (Settings > General > Danger zone) once testing is done.
Token: as publish_release.py (GITHUB_TOKEN, `gh auth token`, or %APPDATA%\\MNRLauncher\\publish_token.txt).
"""
import argparse, json, os, shutil, subprocess, sys, time, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import public_files
import publish_release as pr

ROOT = public_files.ROOT
DEFAULT_REPO = 'quackmeansquack/mnr-pc-public'
WORK = os.path.join(ROOT, 'launcher', 'data', 'publish_public_tmp')
CLONE = os.path.join(ROOT, 'launcher', 'data', 'public_repo')
APP = os.path.join(ROOT, 'launcher', 'app_public', 'MNRLauncher.exe')
README = """# ModNation Racers PC (public edition)

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
"""
GITIGNORE = "bin/\nobj/\n.vs/\n*.user\n"


def build_launcher(version):
    src = os.path.join(ROOT, 'launcher', 'MNRLauncher')
    asm = '.'.join(str(int(x)) for x in version.split('.'))
    print(f'building the public launcher {version}...', flush=True)
    r = subprocess.run(['dotnet', 'publish', '-c', 'Release', '-p:Edition=Public', '-r', 'win-x64', '--self-contained', 'true',
                        '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-p:EnableCompressionInSingleFile=true',
                        '-p:DebugType=none', f'-p:Version={asm}', f'-p:InformationalVersion={version}', '-o', os.path.dirname(APP)],
                       cwd=src, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('Launcher build failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


def git(*a, cwd=CLONE, check=True):
    r = subprocess.run(['git', *a], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f'git {" ".join(a)} failed:\n{r.stdout}{r.stderr}')
    return r.stdout.strip()


def push_source(repo, tok, version, notes):
    url = f'https://x-access-token:{tok}@github.com/{repo}.git'
    if not os.path.isdir(os.path.join(CLONE, '.git')):
        os.makedirs(os.path.dirname(CLONE), exist_ok=True)
        git('clone', url, CLONE, cwd=os.path.dirname(CLONE))
    git('remote', 'set-url', 'origin', url)
    git('pull', '--ff-only', check=False)
    want = set(public_files.source_files())
    for d, dirs, fs in os.walk(CLONE):   # files no longer published leave the repository
        dirs[:] = [x for x in dirs if x != '.git']
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), CLONE).replace('\\', '/')
            if rel not in want and rel not in ('README.md', '.gitignore'):
                os.remove(os.path.join(d, f))
    for rel in sorted(want):
        to = os.path.join(CLONE, rel)
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, rel), to)
    open(os.path.join(CLONE, 'README.md'), 'w', newline='\n').write(README)
    open(os.path.join(CLONE, '.gitignore'), 'w', newline='\n').write(GITIGNORE)
    git('add', '-A')
    if not git('status', '--porcelain'):
        print('source: no changes')
        return
    git('-c', 'user.name=MNR PC', '-c', 'user.email=noreply@users.noreply.github.com', 'commit', '-m', f'v{version}' + (f': {notes}' if notes else ''))
    git('push', 'origin', 'HEAD')
    print('source pushed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=DEFAULT_REPO)
    ap.add_argument('--notes', default='')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--no-source', action='store_true', help="don't push the source tree")
    ap.add_argument('--keep-launcher', action='store_true', help="the release keeps the previous release's launcher")
    args = ap.parse_args()
    pr.REPO = repo = args.repo
    version = time.strftime('%Y.%m.%d.%H%M')
    tok = pr.token()

    st, info = pr.api('GET', f'/repos/{repo}', tok)
    if st == 404 and args.dry_run:
        print(f'{repo} does not exist yet (a real run creates it, private)')
    elif st == 404:
        owner, name = repo.split('/', 1)
        st, info = pr.api('POST', '/user/repos', tok, {'name': name, 'private': True, 'auto_init': True,
                                                        'description': 'ModNation Racers PC, made from your own copy of the game'})
        if st != 201:
            sys.exit(f'Could not create {repo}: {info}')
        print(f'created private repository {repo}')
    elif st != 200:
        sys.exit(f'GitHub: {st} {info}')
    else:
        print(f"{repo}: {'private' if info.get('private') else 'PUBLIC'}")

    files = public_files.runtime_files()
    prev = None
    st, rel = pr.api('GET', f'/repos/{repo}/releases/latest', tok)
    if st == 200:
        m = next((a for a in rel['assets'] if a['name'] == 'manifest.json'), None)
        if m:
            prev = json.loads(pr.get_asset(m['url'], tok))
    entries = [{'path': f, 'size': os.path.getsize(os.path.join(ROOT, f)), 'sha256': pr.sha256(os.path.join(ROOT, f))} for f in files]
    old = prev['packs'][0] if prev and prev.get('packs') else None
    rt_changed = not old or sorted((e['path'], e['sha256']) for e in old['files']) != sorted((e['path'], e['sha256']) for e in entries)
    if args.keep_launcher and not (prev and prev.get('launcher')):
        sys.exit('--keep-launcher needs a previous release with a launcher.')
    print(f"{len(files)} runtime files ({'changed' if rt_changed else 'unchanged'})")
    if args.dry_run:
        for f in files: print('  ', f)
        return
    if not args.no_source:
        push_source(repo, tok, version, args.notes)
    if not args.keep_launcher:
        build_launcher(version)
    lsha = prev['launcher']['sha256'] if args.keep_launcher else pr.sha256(APP)
    l_changed = not prev or not prev.get('launcher') or prev['launcher']['sha256'] != lsha
    if not rt_changed and not l_changed:
        print('Nothing changed in the release.')
        return

    st, draft = pr.api('POST', f'/repos/{repo}/releases', tok, {'tag_name': f'v{version}', 'name': f'pending v{version}', 'draft': True,
                                                                'body': args.notes or 'Update'})
    if st != 201:
        sys.exit(f'Could not create the release: {draft}')
    tag = f'v{version}'
    os.makedirs(WORK, exist_ok=True)
    manifest = {'version': version, 'notes': args.notes, 'packs': []}
    if rt_changed:
        zp = os.path.join(WORK, 'runtime.zip')
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in files:
                z.write(os.path.join(ROOT, f), f)
        st, draft = pr.api('GET', f"/repos/{repo}/releases/{draft['id']}", tok)
        pr.upload(tok, draft, zp, 'runtime.zip')
        manifest['packs'].append({'name': 'runtime', 'files': entries, 'release': tag, 'asset': 'runtime.zip', 'size': os.path.getsize(zp), 'sha256': pr.sha256(zp)})
        os.remove(zp)
    else:
        manifest['packs'].append(old)
    if l_changed:
        st, draft = pr.api('GET', f"/repos/{repo}/releases/{draft['id']}", tok)
        pr.upload(tok, draft, APP, 'MNRLauncher.exe')
        manifest['launcher'] = {'release': tag, 'asset': 'MNRLauncher.exe', 'size': os.path.getsize(APP), 'sha256': lsha}
    else:
        manifest['launcher'] = prev['launcher']
    mp = os.path.join(WORK, 'manifest.json')
    json.dump(manifest, open(mp, 'w'), indent=1)
    st, draft = pr.api('GET', f"/repos/{repo}/releases/{draft['id']}", tok)
    pr.upload(tok, draft, mp, 'manifest.json')
    st, res = pr.api('PATCH', f"/repos/{repo}/releases/{draft['id']}", tok, {'draft': False, 'name': tag, 'make_latest': 'true'})
    if st != 200:
        sys.exit(f'Could not publish the release: {res}')
    print(f'published {tag}: https://github.com/{repo}/releases/tag/{tag}')


if __name__ == '__main__':
    main()
