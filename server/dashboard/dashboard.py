#!/usr/bin/env python3
"""MNR server dashboard: a small web page that shows the private server's health, players, races, creations and logs.

    python3 dashboard.py            serves http://127.0.0.1:10090 (DASH_HOST / DASH_PORT change that;
                                    MNR_SERVER_NAME: the address players use, e.g. in dashboard/dashboard.env)

Runs as mnr-server inside WSL (start.sh starts it). It only reads: the logs (bombd.log, plgarage.log), the backups
folder and the PLGarage database (SELECT queries, with the server's own account from secrets.env). plgarage.log starts
empty each time PLGarage restarts and has no dates, so the dashboard keeps a dated copy of it in
dashboard/plgarage-history.log. Python standard library only.
"""
import datetime as dt
import json
import os
import re
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

SRV = '/home/mnr-server/srv'
HERE = os.path.dirname(os.path.abspath(__file__))
BOMBD_LOG = os.path.join(SRV, 'bombd.log')
PLG_LOG = os.path.join(SRV, 'plgarage.log')
PLG_HISTORY = os.path.join(HERE, 'plgarage-history.log')
PLG_STATE = os.path.join(HERE, 'plgarage-history.json')
BACKUPS = os.path.join(SRV, 'backups')
SECRETS = os.path.join(SRV, 'secrets.env')
HOST = os.environ.get('DASH_HOST', '127.0.0.1')
PORT = int(os.environ.get('DASH_PORT', '10090'))
SERVER_NAME = os.environ.get('MNR_SERVER_NAME', '')   # the address players use (dashboard.env), shown with its DNS answer

PORTS = [(10050, 'PLGarage web API'), (10501, 'Directory'), (10412, 'Game browser'), (10505, 'Game manager'),
         (10510, 'Matchmaking')]
PROCESSES = [('PLGarage', 'GameServer.dll'), ('Bombd', 'bombd-app/Bombd'), ('MariaDB', 'mariadbd'),
             ('Bombd watchdog', 'bombd-loop.sh'), ('Nightly backups', 'backup-loop.sh')]
CREATION_TYPES = {0: 'Photo', 1: 'Planet', 2: 'Track', 3: 'Item', 4: 'Career track', 5: 'Deleted', 6: 'Character', 7: 'Kart'}
SYSTEM_USER_ID = 1   # the server's own account (career tracks)

lock = threading.Lock()


# ---------------------------------------------------------------- log readers

class Line(dict):
    """One log line: ts (epoch seconds or None), level (info/warn/error), source (service), text, raw."""


BOMBD_RE = re.compile(r'^\[(\d\d)/(\d\d)/(\d{4}) (\d\d):(\d\d):(\d\d)\] \[(\w+)\] \[(\w+)\]: ?(.*)$')
PLG_RE = re.compile(r'^\[(\d\d):(\d\d):(\d\d) (\w{3})\] ?(.*)$')
PLG_DATED_RE = re.compile(r'^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d) (\w{3}) ?(.*)$')
LEVELS = {'Info': 'info', 'INF': 'info', 'Debug': 'debug', 'DBG': 'debug', 'VRB': 'debug', 'Warning': 'warn',
          'WRN': 'warn', 'Error': 'error', 'ERR': 'error', 'Fatal': 'error', 'FTL': 'error'}


class BombdLog:
    """bombd.log only grows (bombd-loop.sh appends): read the new part each time."""

    def __init__(self):
        self.lines = []
        self.offset = 0
        self.pending = b''

    def update(self):
        try:
            size = os.path.getsize(BOMBD_LOG)
        except OSError:
            return
        if size < self.offset:   # replaced or truncated: read it again
            self.lines, self.offset, self.pending = [], 0, b''
        if size == self.offset:
            return
        with open(BOMBD_LOG, 'rb') as f:
            f.seek(self.offset)
            data = self.pending + f.read(size - self.offset)
        self.offset = size
        parts = data.split(b'\n')
        self.pending = parts.pop()   # an unfinished last line waits for the next read
        for raw in parts:
            text = raw.decode('utf-8', 'replace').rstrip('\r')
            if not text.strip():
                continue
            m = BOMBD_RE.match(text)
            if m:
                d, mo, y, h, mi, s, level, source, msg = m.groups()
                ts = dt.datetime(int(y), int(mo), int(d), int(h), int(mi), int(s)).timestamp()
                self.lines.append(Line(ts=ts, level=LEVELS.get(level, 'info'), source=source, text=msg))
            elif text.startswith('[restart]'):
                self.lines.append(Line(ts=self.lines[-1]['ts'] if self.lines else None, level='warn', source='Watchdog', text=text[10:]))
            else:   # a continuation (stack trace and the like)
                prev = self.lines[-1] if self.lines else None
                self.lines.append(Line(ts=prev['ts'] if prev else None, level=prev['level'] if prev else 'info',
                                       source=prev['source'] if prev else '', text=text, cont=True))


class PlgLog:
    """plgarage.log is recreated whenever PLGarage starts and gives times without dates. Dates come from the
    process start (each time going backwards is the next day); every line is kept in plgarage-history.log."""

    def __init__(self):
        self.lines = []
        self.run = None        # the PLGarage start time this run's lines were dated from
        self.done = 0          # lines of the current plgarage.log already taken
        self.day = None
        self.last_secs = None
        self._load()

    def _load(self):
        try:
            st = json.load(open(PLG_STATE))
            self.run, self.done = st.get('run'), st.get('done', 0)
            self.day = dt.date.fromisoformat(st['day']) if st.get('day') else None
            self.last_secs = st.get('last_secs')
        except (OSError, ValueError, KeyError):
            pass
        try:
            with open(PLG_HISTORY, encoding='utf-8', errors='replace') as f:
                for text in f:
                    text = text.rstrip('\n')
                    m = PLG_DATED_RE.match(text)
                    if m:
                        y, mo, d, h, mi, s, lv, msg = m.groups()
                        ts = dt.datetime(int(y), int(mo), int(d), int(h), int(mi), int(s)).timestamp()
                        self.lines.append(Line(ts=ts, level=LEVELS.get(lv, 'info'), source='PLGarage', text=msg))
                    elif self.lines:
                        p = self.lines[-1]
                        self.lines.append(Line(ts=p['ts'], level=p['level'], source='PLGarage', text=text, cont=True))
        except OSError:
            pass

    def _save(self):
        json.dump({'run': self.run, 'done': self.done, 'day': self.day.isoformat() if self.day else None,
                   'last_secs': self.last_secs}, open(PLG_STATE, 'w'))

    def update(self, plg_started):
        if plg_started is None:
            return
        run = int(plg_started)
        if self.run is None or abs(run - self.run) > 5:   # PLGarage started again: a new log
            self.run, self.done = run, 0
            self.day = dt.datetime.fromtimestamp(run).date()
            self.last_secs = None
        try:
            with open(PLG_LOG, encoding='utf-8', errors='replace') as f:
                all_lines = f.read().split('\n')
        except OSError:
            return
        if all_lines and all_lines[-1] == '':
            all_lines.pop()
        else:
            all_lines = all_lines[:-1]   # (the last line may still be being written)
        new = all_lines[self.done:]
        if not new:
            return
        out = []
        for text in new:
            text = text.rstrip('\r')
            m = PLG_RE.match(text)
            if m:
                h, mi, s, lv, msg = m.groups()
                secs = int(h) * 3600 + int(mi) * 60 + int(s)
                if self.last_secs is not None and secs < self.last_secs - 60:
                    self.day += dt.timedelta(days=1)
                self.last_secs = secs
                stamp = dt.datetime.combine(self.day, dt.time(int(h), int(mi), int(s)))
                out.append(f'{stamp:%Y-%m-%d %H:%M:%S} {lv} {msg}')
                self.lines.append(Line(ts=stamp.timestamp(), level=LEVELS.get(lv, 'info'), source='PLGarage', text=msg))
            elif text.strip():
                out.append(text)
                p = self.lines[-1] if self.lines else None
                self.lines.append(Line(ts=p['ts'] if p else None, level=p['level'] if p else 'info', source='PLGarage',
                                       text=text, cont=True))
        self.done = len(all_lines)
        with open(PLG_HISTORY, 'a', encoding='utf-8') as f:
            f.write(''.join(x + '\n' for x in out))
        self._save()


bombd = BombdLog()
plg = PlgLog()


# ---------------------------------------------------------------- system state

def processes():
    out = []
    for name, pattern in PROCESSES:
        try:
            pids = subprocess.run(['pgrep', '-f', pattern], capture_output=True, text=True).stdout.split()
        except OSError:
            pids = []
        pids = [p for p in pids if int(p) != os.getpid()]
        up = None
        if pids:
            r = subprocess.run(['ps', '-o', 'etimes=', '-p', pids[0]], capture_output=True, text=True).stdout.strip()
            up = int(r) if r.isdigit() else None
        out.append({'name': name, 'running': bool(pids), 'pid': int(pids[0]) if pids else None,
                    'started': time.time() - up if up is not None else None})
    return out


def ports():
    out = []
    for port, name in PORTS:
        s = socket.socket()
        s.settimeout(0.5)
        try:
            s.connect(('127.0.0.1', port))
            ok = True
        except OSError:
            ok = False
        finally:
            s.close()
        out.append({'port': port, 'name': name, 'open': ok})
    return out


def dns():
    if not SERVER_NAME:
        return None
    try:
        return socket.gethostbyname(SERVER_NAME)
    except OSError:
        return None


def backups():
    out = {'list': [], 'log': []}
    try:
        for d in sorted(os.listdir(BACKUPS), reverse=True):
            p = os.path.join(BACKUPS, d)
            if os.path.isdir(p) and d[:2] == '20':
                size = sum(os.path.getsize(os.path.join(p, f)) for f in os.listdir(p))
                out['list'].append({'name': d, 'size': size, 'time': os.path.getmtime(p)})
        with open(os.path.join(BACKUPS, 'backup.log')) as f:
            out['log'] = [x.rstrip() for x in f.readlines()[-10:]]
    except OSError:
        pass
    return out


def disk():
    st = os.statvfs(SRV)
    return {'free': st.f_bavail * st.f_frsize, 'total': st.f_blocks * st.f_frsize}


# ---------------------------------------------------------------- database

def db_password():
    try:
        for line in open(SECRETS):
            if line.startswith('DBPW='):
                return line.split('=', 1)[1].strip().strip('"\'')
    except OSError:
        pass
    return None


DB_QUERIES = {   # name: (table, columns)
    'users': ('Users', 'UserId Username CreatedAt IsBanned PlayedMNR ModMiles WinStreak LongestWinStreak'),
    'started': ('OnlineRacesStarted', 'PlayerId StartedAt'),
    'finished': ('OnlineRacesFinished', 'PlayerId FinishedAt IsWinner'),
    'creations': ('PlayerCreations', 'PlayerCreationId Name Type PlayerId CreatedAt ParentCreationId RacesFinished'),
    'downloads': ('PlayerCreationDownloads', 'PlayerCreationId PlayerId DownloadedAt'),
    'sessions': ('Sessions', 'UserId Presence LastPing IsRpcn'),
}


def db():
    pw = db_password()
    sep = '__SPLIT__'
    sql = ''.join(f"select {', '.join(cols.split())} from {table}; select '{sep}';" for table, cols in DB_QUERIES.values())
    env = dict(os.environ, MYSQL_PWD=pw or '')
    try:
        r = subprocess.run(['mariadb', '-h', '127.0.0.1', '-u', 'plgarage', '--batch', '--raw', '--skip-column-names',
                            'PLGarage', '-e', sql], capture_output=True, text=True, env=env, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    if r.returncode != 0:
        return None, r.stderr.strip()[:300]
    out, block = {}, []
    names = iter(DB_QUERIES.items())
    for line in r.stdout.split('\n'):
        if line == sep:
            name, (table, cols) = next(names)
            head = cols.split()
            out[name] = [dict(zip(head, x.split('\t'))) for x in block if x != '']
            block = []
        else:
            block.append(line)
    return out, None


def ts_of(s):
    try:
        return dt.datetime.fromisoformat(s[:19]).timestamp()
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------- analysis

JOIN_RE = re.compile(r'^(\S+) has been (connected|disconnected)\.$')
MIGRATE_RE = re.compile(r'^(\S+) migrated to (\S+?)\.?$')
SIM_RE = re.compile(r'^Starting SimServer \((\w+):')
SYNC_RE = re.compile(r'^Syncing session from Web API for (\S+)')
IMPORT_RE = re.compile(r'^\[mnrpc\] imported (\w+) (\d+) "(.*)" by (\S+) from (\S+)(?: \(remix of (\d+)\))?')


def analyse(data, now):
    """Players, sessions, daily activity and the event feed from bombd.log (plus PLGarage's imports)."""
    players = {}
    sessions = []          # finished and open play sessions (game manager connection)
    open_sessions = {}
    in_room = {}
    events = []
    bombd_starts = []
    lobbies = []
    for ln in bombd.lines:
        if ln.get('cont'):
            continue
        t, src, text = ln['ts'], ln['source'], ln['text']
        if src == 'Directory' and text.startswith('Initializing service'):
            bombd_starts.append(t)
            for name, start in open_sessions.items():   # Bombd restarted: everyone was dropped
                sessions.append({'player': name, 'start': start, 'end': t, 'cut': True})
            open_sessions.clear()
            in_room.clear()
            events.append({'ts': t, 'kind': 'server', 'text': 'Bombd started'})
            continue
        m = JOIN_RE.match(text)
        if m and src == 'GameManager':
            name, what = m.groups()
            p = players.setdefault(name, {'name': name, 'first': t, 'last': t, 'sessions': 0, 'seconds': 0, 'days': set(), 'rooms': 0})
            p['last'] = t
            p['days'].add(dt.datetime.fromtimestamp(t).date().isoformat())
            if what == 'connected':
                if name in open_sessions:
                    sessions.append({'player': name, 'start': open_sessions[name], 'end': t, 'cut': True})
                open_sessions[name] = t
                p['sessions'] += 1
                events.append({'ts': t, 'kind': 'join', 'player': name, 'text': f'{name} came online'})
            elif name in open_sessions:
                start = open_sessions.pop(name)
                sessions.append({'player': name, 'start': start, 'end': t})
                in_room.pop(name, None)
                events.append({'ts': t, 'kind': 'leave', 'player': name, 'text': f'{name} went offline', 'duration': t - start})
            continue
        m = MIGRATE_RE.match(text)
        if m and src == 'GameServer':
            name, room = m.groups()
            in_room[name] = room
            if name in players:
                players[name]['rooms'] += 1
            kind = 'Competitive race' if 'competitive' in room else 'Kart park' if 'park' in room.lower() else 'Game room'
            events.append({'ts': t, 'kind': 'race', 'player': name, 'text': f'{name} joined a {kind.lower()}'})
            continue
        m = SIM_RE.match(text)
        if m:
            lobbies.append({'ts': t, 'kind': m.group(1)})
            continue
        if ln['level'] in ('warn', 'error') and src != 'SslConnection':
            events.append({'ts': t, 'kind': ln['level'], 'text': f'{src}: {text}'})
    for name, start in open_sessions.items():
        sessions.append({'player': name, 'start': start, 'end': None})
    for s in sessions:
        end = s['end'] if s['end'] is not None else now
        players[s['player']]['seconds'] += max(0, end - s['start'])

    # PLGarage: imports, starts, errors
    imports = []
    for ln in plg.lines:
        if ln.get('cont'):
            continue
        m = IMPORT_RE.match(ln['text'])
        if m:
            kind, cid, name, author, origin, remix = m.groups()
            imports.append({'ts': ln['ts'], 'type': kind.title(), 'id': int(cid), 'name': name.strip(), 'author': author,
                            'origin': origin, 'remix_of': int(remix) if remix else None})
            events.append({'ts': ln['ts'], 'kind': 'import', 'text': f'Imported {kind.lower()} "{name.strip()}" by {author}'})
        elif ln['text'].startswith('Application started'):
            events.append({'ts': ln['ts'], 'kind': 'server', 'text': 'PLGarage started'})
        elif ln['level'] in ('warn', 'error'):
            events.append({'ts': ln['ts'], 'kind': ln['level'], 'text': 'PLGarage: ' + ln['text'][:200]})

    # the database: accounts, races, creations
    users = {u['UserId']: u for u in (data or {}).get('users', [])}
    by_name = {u['Username']: u for u in users.values()}
    started = (data or {}).get('started', [])
    finished = (data or {}).get('finished', [])
    creations = [c for c in (data or {}).get('creations', []) if c['PlayerId'] != str(SYSTEM_USER_ID) and c['Type'] != '5']
    downloads = (data or {}).get('downloads', [])
    races_by = {}
    for r in started:
        races_by.setdefault(r['PlayerId'], {'started': 0, 'finished': 0, 'wins': 0})['started'] += 1
    for r in finished:
        e = races_by.setdefault(r['PlayerId'], {'started': 0, 'finished': 0, 'wins': 0})
        e['finished'] += 1
        e['wins'] += r['IsWinner'] == '1'
    made_by = {}
    for c in creations:
        made_by[c['PlayerId']] = made_by.get(c['PlayerId'], 0) + 1
    races = {}   # one row per player per race: group by start time
    for r in started:
        races.setdefault(r['StartedAt'][:19], set()).add(r['PlayerId'])
    for when, ids in races.items():
        names = ', '.join(users[i]['Username'] if i in users else '?' for i in sorted(ids))
        events.append({'ts': ts_of(when), 'kind': 'race', 'text': f'Online race started: {names}'})

    player_rows = []
    for name, p in players.items():
        u = by_name.get(name)
        rb = races_by.get(u['UserId'], {}) if u else {}
        player_rows.append({
            'name': name, 'first': p['first'], 'last': p['last'], 'sessions': p['sessions'], 'seconds': round(p['seconds']),
            'days': len(p['days']), 'online': name in open_sessions, 'room': in_room.get(name),
            'account': ts_of(u['CreatedAt']) if u else None, 'banned': bool(u and u['IsBanned'] == '1'),
            'races': rb.get('started', 0), 'finished': rb.get('finished', 0), 'wins': rb.get('wins', 0),
            'creations': made_by.get(u['UserId'], 0) if u else 0,
        })
    player_rows.sort(key=lambda r: -r['last'])

    # accounts that never came online: authors of imported creations, or players who only used the web side
    authors = []
    for u in users.values():
        if u['Username'] in players or u['UserId'] == str(SYSTEM_USER_ID):
            continue
        authors.append({'name': u['Username'], 'created': ts_of(u['CreatedAt']), 'creations': made_by.get(u['UserId'], 0),
                        'races': races_by.get(u['UserId'], {}).get('started', 0)})
    authors.sort(key=lambda r: -(r['created'] or 0))
    logged = {i['author'] for i in imports}
    for a in authors:   # (PLGarage's log from before a restart is gone: the account's creation marks the import)
        if a['created'] and a['creations'] and a['name'] not in logged:
            events.append({'ts': a['created'], 'kind': 'import', 'player': a['name'], 'count': a['creations'],
                           'text': f'Imported {a["creations"]} creation{"s" if a["creations"] != 1 else ""} by {a["name"]}'})

    # the last 30 days
    today = dt.datetime.fromtimestamp(now).date()
    days = [(today - dt.timedelta(days=i)).isoformat() for i in range(29, -1, -1)]
    daily = {d: {'date': d, 'players': set(), 'sessions': 0, 'races': 0, 'imports': 0, 'seconds': 0} for d in days}
    for s in sessions:
        d = dt.datetime.fromtimestamp(s['start']).date().isoformat()
        if d in daily:
            daily[d]['players'].add(s['player'])
            daily[d]['sessions'] += 1
            daily[d]['seconds'] += max(0, (s['end'] or now) - s['start'])
    for when in races:
        d = when[:10]
        if d in daily:
            daily[d]['races'] += 1
    for e in events:
        if e['kind'] == 'import' and e.get('ts'):
            d = dt.datetime.fromtimestamp(e['ts']).date().isoformat()
            if d in daily:
                daily[d]['imports'] += e.get('count', 1)
    daily_rows = [dict(v, players=sorted(v['players']), seconds=round(v['seconds'])) for v in daily.values()]

    type_counts = {}
    for c in creations:
        t = CREATION_TYPES.get(int(c['Type']), 'Other')
        type_counts[t] = type_counts.get(t, 0) + 1
    recent_creations = sorted(creations, key=lambda c: c['CreatedAt'], reverse=True)[:40]
    recent_creations = [{'id': int(c['PlayerCreationId']), 'name': c['Name'], 'type': CREATION_TYPES.get(int(c['Type']), '?'),
                         'author': users.get(c['PlayerId'], {}).get('Username', '?'), 'created': ts_of(c['CreatedAt']),
                         'races': int(c['RacesFinished'] or 0)} for c in recent_creations]
    dl = [{'id': int(d['PlayerCreationId']), 'player': users.get(d['PlayerId'], {}).get('Username', '?'),
           'ts': ts_of(d['DownloadedAt'])} for d in downloads]
    names = {int(c['PlayerCreationId']): c['Name'] for c in (data or {}).get('creations', [])}
    for d in dl:
        d['name'] = names.get(d['id'], f'#{d["id"]}')
        events.append({'ts': d['ts'], 'kind': 'download', 'text': f'{d["player"]} downloaded "{d["name"]}"'})

    events = [e for e in events if e.get('ts')]
    events.sort(key=lambda e: -e['ts'])
    week = now - 7 * 86400
    return {
        'players': player_rows,
        'authors': authors,
        'online': [{'name': n, 'since': t, 'room': in_room.get(n)} for n, t in sorted(open_sessions.items(), key=lambda x: x[1])],
        'daily': daily_rows,
        'events': events[:400],
        'sessions': sorted(sessions, key=lambda s: -s['start'])[:200],
        'imports': sorted(imports, key=lambda i: -i['ts'])[:200],
        'creations': {'total': len(creations), 'types': type_counts, 'recent': recent_creations,
                      'downloads': sorted(dl, key=lambda d: -(d['ts'] or 0))[:100]},
        'totals': {
            'accounts': len([u for u in users.values() if u['UserId'] != str(SYSTEM_USER_ID)]),
            'players': len(players),
            'players_week': len({s['player'] for s in sessions if s['start'] >= week}),
            'races': len(races),
            'races_week': len([w for w in races if (ts_of(w) or 0) >= week]),
            'hours': round(sum(p['seconds'] for p in players.values()) / 3600, 1),
            'imports_week': sum(e.get('count', 1) for e in events if e['kind'] == 'import' and e['ts'] >= week),
            'bombd_starts': len(bombd_starts),
            'last_bombd_start': bombd_starts[-1] if bombd_starts else None,
        },
        'lobbies': lobbies[-50:],
    }


# ---------------------------------------------------------------- API

cache = {'t': 0, 'data': None}


def summary():
    with lock:
        if cache['data'] and time.time() - cache['t'] < 4:
            return cache['data']
        now = time.time()
        procs = processes()
        plg_started = next((p['started'] for p in procs if p['name'] == 'PLGarage'), None)
        bombd.update()
        plg.update(plg_started)
        data, db_error = db()
        out = analyse(data, now)
        out.update({'now': now, 'server': SERVER_NAME or socket.gethostname(), 'processes': procs, 'ports': ports(), 'dns': dns(),
                    'backups': backups(), 'disk': disk(), 'db_error': db_error,
                    'log_sizes': {'bombd': len(bombd.lines), 'plgarage': len(plg.lines)}})
        cache.update(t=now, data=out)
        return out


def log_page(src, after, limit, level, text, service, noise):
    with lock:
        bombd.update()
        lines = bombd.lines if src == 'bombd' else plg.lines
        n = len(lines)
        picked = []
        i = n - 1
        text = (text or '').lower()
        while i >= 0 and len(picked) < limit:
            ln = lines[i]
            if after is not None and i <= after:
                break
            ok = True
            if level and level != 'all':
                ok = ln['level'] == level or (level == 'warn' and ln['level'] == 'error')
            if ok and service and ln['source'] != service:
                ok = False
            if ok and not noise and src == 'bombd' and (ln['text'].startswith('HandleServiceTransaction') or
                                                        ln['text'].startswith('-> Registered') or
                                                        ln['text'].startswith('-> UID is')):
                ok = False
            if ok and text and text not in ln['text'].lower() and text not in ln['source'].lower():
                ok = False
            if ok:
                picked.append(dict(ln, i=i))
            i -= 1
        picked.reverse()
        services = sorted({ln['source'] for ln in lines if ln['source']})
        return {'lines': picked, 'last': n - 1, 'services': services, 'more': i >= 0 and after is None}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype):
        b = body if isinstance(body, bytes) else body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path in ('/', '/index.html'):
                with open(os.path.join(HERE, 'index.html'), 'rb') as f:
                    self.send(200, f.read(), 'text/html; charset=utf-8')
            elif u.path == '/api/summary':
                self.send(200, json.dumps(summary()), 'application/json')
            elif u.path == '/api/log':
                src = 'plgarage' if q.get('src') == 'plgarage' else 'bombd'
                after = int(q['after']) if q.get('after', '').lstrip('-').isdigit() else None
                limit = max(1, min(int(q.get('limit', '500')), 5000))
                self.send(200, json.dumps(log_page(src, after, limit, q.get('level'), q.get('q'), q.get('service'),
                                                   q.get('noise') == '1')), 'application/json')
            else:
                self.send(404, 'not found', 'text/plain')
        except BrokenPipeError:
            pass
        except Exception as e:   # (shown on the page instead of a dropped connection)
            self.send(500, json.dumps({'error': f'{type(e).__name__}: {e}'}), 'application/json')


def main():
    summary()   # read everything once before the first visit
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f'MNR dashboard on http://{HOST}:{PORT}', flush=True)
    httpd.serve_forever()


if __name__ == '__main__':
    main()
