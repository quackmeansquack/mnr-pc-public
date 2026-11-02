"""Publish the build to the private GitHub repository the launcher installs from (Install & updates).

    python -I launcher\\publish_release.py [--notes "what changed"] [--repo owner/name] [--dry-run] [--build-launcher] [--keep-launcher]

The launcher is rebuilt first (dotnet publish, stamped with the release version) whenever its source changed since
launcher/app/MNRLauncher.exe was built; every PC's launcher then offers the update at start ("Update available").

What it uploads (paths relative to the MNR folder, as installed):
    re/recomp/out_v111/mnr.exe, march.txt   the game (universal build)
    re/bin_v111/EBOOT.elf, patches/...      the 1.11 executable and data patches
    re/shadercache/                         compiled shaders
    extracted/PS3_GAME/                     disc contents
    hdd0/game/BCUS98167/                    installed game data and the 1.11 update
    launcher/app/MNRLauncher.exe            the launcher (self-updating; the "launcher" entry)
Never: saves, creations, settings, logs, mods (the player's own data).

Files are grouped into zip "packs" of at most ~1.9 GB (GitHub's asset limit is 2 GB). Each release holds manifest.json
listing every pack, its files' sizes and SHA-256, and the release where the pack's zip lives: a new release uploads only
the packs whose files changed (and the launcher if it changed); unchanged packs keep pointing at older releases. Files
stay in the pack they were first put in, so one changed file re-uploads one pack, not everything after it.

Token (needs Contents: read & write on the repository; Administration: write too if the script should create it):
GITHUB_TOKEN in the environment, else `gh auth token` (GitHub CLI, after `gh auth login`), else the first line of
%APPDATA%\\MNRLauncher\\publish_token.txt. An interrupted upload is resumed by running the script again (the release
stays a draft until every asset is up).
"""
import argparse, hashlib, json, os, subprocess, sys, time, urllib.error, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # MNR
DEFAULT_REPO_FILE = os.path.join(os.environ.get('APPDATA', ''), 'MNRLauncher', 'publish_repo.txt')
CACHE = os.path.join(ROOT, 'launcher', 'data', 'publish_hashes.json')
WORK = os.path.join(ROOT, 'launcher', 'data', 'publish_tmp')
PACK_LIMIT = 1_900_000_000
GROUPS = [   # (pack name prefix, paths: files or folders)
    ('game', ['re/recomp/out_v111/mnr.exe', 're/recomp/out_v111/march.txt', 're/bin_v111/EBOOT.elf', 'patches/patches_v111.txt']),   # (no re/shadercache: each PC compiles its own)
    ('disc', ['extracted/PS3_GAME']),
    ('hdd', ['hdd0/game/BCUS98167']),
]
LAUNCHER = 'launcher/app/MNRLauncher.exe'
API = 'https://api.github.com'


def token():
    t = os.environ.get('GITHUB_TOKEN', '').strip()
    if t:
        return t
    try:
        t = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True, timeout=30).stdout.strip()
        if t:
            return t
    except (OSError, subprocess.SubprocessError):
        pass
    f = os.path.join(os.environ.get('APPDATA', ''), 'MNRLauncher', 'publish_token.txt')
    if os.path.exists(f):
        return open(f).readline().strip()
    sys.exit('No GitHub token: set GITHUB_TOKEN, sign in with the GitHub CLI (gh auth login), or put one in ' + f)


def api(method, url, tok, body=None, data=None, headers=None, ok=(200, 201, 204)):
    h = {'Authorization': f'Bearer {tok}', 'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28',
         'User-Agent': 'mnr-publish'}
    if headers:
        h.update(headers)
    if body is not None:
        data = json.dumps(body).encode()
        h['Content-Type'] = 'application/json'
    req = urllib.request.Request(url if url.startswith('http') else API + url, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=3600) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw and r.headers.get('Content-Type', '').startswith('application/json') else raw)
    except urllib.error.HTTPError as e:
        raw = e.read()
        if e.code in ok:
            return e.code, None
        return e.code, raw.decode(errors='replace')


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def get_asset(url, tok):
    """An asset's bytes: GitHub answers with a redirect to storage that must be fetched without the token."""
    req = urllib.request.Request(url, headers={'Authorization': f'Bearer {tok}', 'Accept': 'application/octet-stream',
                                               'User-Agent': 'mnr-publish'})
    try:
        with urllib.request.build_opener(_NoRedirect).open(req, timeout=600) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308):
            with urllib.request.urlopen(urllib.request.Request(e.headers['Location'], headers={'User-Agent': 'mnr-publish'}), timeout=600) as r:
                return r.read()
        raise


EXCLUDE = ('/LOADTABLECACHE/', '.CORRUPT', '/DATA/COMPOSITES/',   # written by the game while it runs (caches it rebuilds itself)
           '/DATA/INSTALL/CHECKFLAG.BIN',   # the game's own install stamp (written on each PC)
           '/USRDIR/DRM-')   # the player's own DLC licenses (Install DLC package): never uploaded; matched ignoring case
# Also rewritten by the game while it runs (kart and track stats, decal composites): a change here alone doesn't re-upload a
# 2 GB pack; the published copy stays as it was (a pack that changes for another reason takes the current file).
STICKY = ('/DATA/ROM/VEHICLE/', '/DATA/COMPOSITES/', '/DATA/DLCSTATUES.XML', '.STAT',   # .STAT: per-track/kart race stats
          '/DATA/CREATIONKEYWORDTAGS.XML', '/DATA/PROFANITYFILTER.XML')   # the tag list and chat filter, saved from the server


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def collect():
    """{group: [(relpath, abspath)]} in a stable order."""
    out = {}
    for name, paths in GROUPS:
        files = []
        for p in paths:
            ap = os.path.join(ROOT, p)
            if os.path.isfile(ap):
                files.append((p, ap))
            elif os.path.isdir(ap):
                for d, dirs, fs in os.walk(ap):
                    dirs.sort()
                    for f in sorted(fs):
                        full = os.path.join(d, f)
                        rel = os.path.relpath(full, ROOT).replace('\\', '/')
                        if not any(x in rel.upper() for x in EXCLUDE):
                            files.append((rel, full))
            else:
                sys.exit(f'Missing: {p}')
        out[name] = files
    return out


def hashes(groups):
    cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}
    res, n = {}, 0
    for files in groups.values():
        for rel, ap in files:
            st = os.stat(ap)
            key = f'{rel}|{st.st_size}|{st.st_mtime_ns}'
            if key not in cache:
                cache[key] = sha256(ap)
                n += 1
            res[rel] = (st.st_size, cache[key])
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(cache, open(CACHE, 'w'))
    print(f'hashed {n} new or changed files')
    return res


def plan(groups, hs, prev):
    """Packs for this release: [{name, files:[...], changed}], keeping files where the previous manifest had them."""
    prev_packs = {p['name']: p for p in prev.get('packs', [])} if prev else {}
    where = {f['path']: p['name'] for p in prev_packs.values() for f in p['files']}
    packs = []
    for gname, files in groups.items():
        mine = {}   # pack name -> [rel]
        new = []
        for rel, _ in files:
            pn = where.get(rel)
            if pn and pn.startswith(gname + '-'):
                mine.setdefault(pn, []).append(rel)
            else:
                new.append(rel)
        # new files: fill the group's last pack, then open more
        names = sorted(mine) or []
        def size_of(lst): return sum(hs[r][0] for r in lst)
        cur = names[-1] if names else None
        idx = int(cur.split('-')[-1]) if cur else 0
        for rel in new:
            if cur is None or size_of(mine[cur]) + hs[rel][0] > PACK_LIMIT:
                idx += 1
                cur = f'{gname}-{idx:02d}'
                mine[cur] = []
            mine[cur].append(rel)
        for pn in sorted(mine):
            files_now = [{'path': r, 'size': hs[r][0], 'sha256': hs[r][1]} for r in mine[pn]]
            old = prev_packs.get(pn)
            same = old is not None and sorted((f['path'], f['sha256']) for f in old['files']) == sorted((f['path'], f['sha256']) for f in files_now)
            packs.append({'name': pn, 'files': files_now, 'changed': not same, 'old': old})
    return packs


def make_zip(pack, path):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for f in pack['files']:
            z.write(os.path.join(ROOT, f['path']), f['path'])


class Progress:
    def __init__(self, f, total, label):
        self.f, self.total, self.label, self.done, self.t = f, total, label, 0, 0

    def read(self, n=-1):
        b = self.f.read(min(n if n > 0 else 1 << 20, 1 << 20))
        self.done += len(b)
        if time.time() - self.t > 5 or not b:
            self.t = time.time()
            print(f'  {self.label}: {self.done / 1e6:,.0f} / {self.total / 1e6:,.0f} MB', flush=True)
        return b

    def __len__(self):
        return self.total


def upload(tok, release, path, name):
    for a in release.get('assets', []):
        if a['name'] == name:
            if a['size'] == os.path.getsize(path) and a.get('state') == 'uploaded':
                print(f'  {name}: already uploaded')
                return a
            api('DELETE', f"/repos/{REPO}/releases/assets/{a['id']}", tok)
    size = os.path.getsize(path)
    url = release['upload_url'].split('{')[0] + '?name=' + urllib.request.quote(name)
    for attempt in range(3):
        with open(path, 'rb') as f:
            st, res = api('POST', url, tok, data=Progress(f, size, name),
                          headers={'Content-Type': 'application/octet-stream', 'Content-Length': str(size)})
        if st == 201:
            return res
        print(f'  upload failed ({st}): {str(res)[:200]}; retrying')
        time.sleep(10)
    sys.exit(f'Upload of {name} failed.')


def build_launcher(version, force=False):
    """Rebuilds launcher/app/MNRLauncher.exe (single file, self-contained) when its source changed since the last build
    (or with --build-launcher), stamped with the release version: the launcher shows it, and every PC that checks for
    updates gets the new launcher like the game files."""
    src = os.path.join(ROOT, 'launcher', 'MNRLauncher')
    exe = os.path.join(ROOT, LAUNCHER)
    built = os.path.getmtime(exe) if os.path.exists(exe) else 0
    newest = 0
    for d, dirs, fs in os.walk(src):
        dirs[:] = [x for x in dirs if x not in ('bin', 'obj')]
        for f in fs:
            newest = max(newest, os.path.getmtime(os.path.join(d, f)))
    if not force and newest <= built:
        return
    parts = version.split('.')
    asm = '.'.join(str(int(x)) for x in parts)   # 2026.10.18.0825 -> 2026.10.18.825 (assembly versions are numbers)
    print(f'building the launcher {version}...')
    r = subprocess.run(['dotnet', 'publish', '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
                        '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-p:EnableCompressionInSingleFile=true',
                        '-p:DebugType=none', f'-p:Version={asm}', f'-p:InformationalVersion={version}', '-o', os.path.dirname(exe)],
                       cwd=src, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('Launcher build failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


def main():
    global REPO
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', help='owner/name (remembered in %%APPDATA%%\\MNRLauncher\\publish_repo.txt)')
    ap.add_argument('--notes', default='', help='what changed (shown in the launcher)')
    ap.add_argument('--dry-run', action='store_true', help='show what would be uploaded')
    ap.add_argument('--build-launcher', action='store_true', help='rebuild the launcher even if its source did not change')
    ap.add_argument('--keep-launcher', action='store_true', help="game files only: the release keeps the previous release's launcher (launcher/app is neither built nor uploaded)")
    args = ap.parse_args()
    release_version = time.strftime('%Y.%m.%d.%H%M')
    REPO = args.repo or (open(DEFAULT_REPO_FILE).read().strip() if os.path.exists(DEFAULT_REPO_FILE) else '')
    if not REPO or '/' not in REPO:
        sys.exit('Which repository? --repo owner/name')
    os.makedirs(os.path.dirname(DEFAULT_REPO_FILE), exist_ok=True)
    open(DEFAULT_REPO_FILE, 'w').write(REPO)
    tok = token()

    st, repo = api('GET', f'/repos/{REPO}', tok)
    if st == 404:
        owner, name = REPO.split('/', 1)
        st0, me = api('GET', '/user', tok)
        if st0 != 200 or me.get('login', '').lower() != owner.lower():
            sys.exit(f'{REPO} does not exist and the token can only create repositories for its own account.')
        st, repo = api('POST', '/user/repos', tok, {'name': name, 'private': True, 'auto_init': True,
                                                     'description': 'ModNation Racers PC build (private)'})
        if st != 201:
            sys.exit(f'Could not create {REPO}: {repo}')
        print(f'created private repository {REPO}')
    elif st != 200:
        sys.exit(f'GitHub: {st} {repo}')
    if not repo.get('private', False):
        sys.exit(f'{REPO} is public: refusing to upload the game there.')

    prev = None
    st, rel = api('GET', f'/repos/{REPO}/releases/latest', tok)
    if st == 200:
        m = next((a for a in rel['assets'] if a['name'] == 'manifest.json'), None)
        if m:
            prev = json.loads(get_asset(m['url'], tok))
            print(f"previous release {prev['version']}")

    if args.keep_launcher and not (prev and prev.get('launcher')):
        sys.exit('--keep-launcher needs a previous release with a launcher.')
    if not args.dry_run and not args.keep_launcher:
        build_launcher(release_version, args.build_launcher)
    groups = collect()
    hs = hashes(groups)
    real = dict(hs)
    if prev:   # game-written files: compare with what was published, so they alone don't mark a pack changed
        published = {f['path']: (f['size'], f['sha256']) for pk in prev.get('packs', []) for f in pk['files']}
        for r in hs:
            if any(x in r for x in STICKY) and r in published and published[r] != hs[r]:
                hs[r] = published[r]
        # a game-written file that was never published (e.g. a new track's .STAT after a race) stays out
        fresh = {r for r in hs if any(x in r for x in STICKY) and r not in published}
        for g in groups: groups[g] = [(r, ap) for r, ap in groups[g] if r not in fresh]
        for r in fresh: del hs[r]; del real[r]
    packs = plan(groups, hs, prev)
    for p in packs:   # a pack that is rebuilt anyway lists the files as they are now (the zip holds those)
        if p['changed']:
            p['files'] = [{'path': f['path'], 'size': real[f['path']][0], 'sha256': real[f['path']][1]} for f in p['files']]
    lsha = sha256(os.path.join(ROOT, LAUNCHER)) if not args.keep_launcher else prev['launcher']['sha256']
    lchanged = not prev or not prev.get('launcher') or prev['launcher']['sha256'] != lsha
    changed = [p for p in packs if p['changed']]
    print(f"{len(packs)} packs, {len(changed)} to upload ({sum(sum(f['size'] for f in p['files']) for p in changed) / 1e9:.2f} GB before compression)"
          + ('; launcher changed' if lchanged else ''))
    for p in changed:
        print('  ', p['name'], len(p['files']), 'files')
    if args.dry_run:
        return
    if not changed and not lchanged:
        print('Nothing changed.')
        return

    # a draft release (resumable) until everything is up
    version = release_version
    st, rels = api('GET', f'/repos/{REPO}/releases?per_page=30', tok)
    draft = next((r for r in rels if r.get('draft') and r.get('name', '').startswith('pending ')), None) if st == 200 else None
    if draft:
        version = draft['tag_name'][1:]
        print(f'resuming draft v{version}')
    else:
        st, draft = api('POST', f'/repos/{REPO}/releases', tok, {'tag_name': f'v{version}', 'name': f'pending v{version}', 'draft': True,
                                                                  'body': args.notes or 'Build update'})
        if st != 201:
            sys.exit(f'Could not create the release: {draft}')
    tag = f'v{version}'

    os.makedirs(WORK, exist_ok=True)
    manifest = {'version': version, 'notes': args.notes, 'packs': []}
    for p in packs:
        entry = {'name': p['name'], 'files': p['files']}
        if p['changed']:
            zp = os.path.join(WORK, p['name'] + '.zip')
            if not os.path.exists(zp + '.ok'):
                print(f"packing {p['name']}...", flush=True)
                make_zip(p, zp)
                open(zp + '.ok', 'w').write(sha256(zp))
            entry.update(release=tag, asset=p['name'] + '.zip', size=os.path.getsize(zp), sha256=open(zp + '.ok').read())
            st, draft = api('GET', f"/repos/{REPO}/releases/{draft['id']}", tok)
            upload(tok, draft, zp, entry['asset'])
            os.remove(zp)
            os.remove(zp + '.ok')
        else:
            o = p['old']
            entry.update(release=o['release'], asset=o['asset'], size=o['size'], sha256=o['sha256'])
        manifest['packs'].append(entry)
    lpath = os.path.join(ROOT, LAUNCHER)
    if lchanged:
        st, draft = api('GET', f"/repos/{REPO}/releases/{draft['id']}", tok)
        upload(tok, draft, lpath, 'MNRLauncher.exe')
        manifest['launcher'] = {'release': tag, 'asset': 'MNRLauncher.exe', 'size': os.path.getsize(lpath), 'sha256': lsha}
    else:
        manifest['launcher'] = prev['launcher']
    mp = os.path.join(WORK, 'manifest.json')
    json.dump(manifest, open(mp, 'w'), indent=1)
    st, draft = api('GET', f"/repos/{REPO}/releases/{draft['id']}", tok)
    upload(tok, draft, mp, 'manifest.json')
    st, res = api('PATCH', f"/repos/{REPO}/releases/{draft['id']}", tok, {'draft': False, 'name': tag, 'make_latest': 'true'})
    if st != 200:
        sys.exit(f'Could not publish the release: {res}')
    print(f'published {tag}: https://github.com/{REPO}/releases/tag/{tag}')


if __name__ == '__main__':
    main()
