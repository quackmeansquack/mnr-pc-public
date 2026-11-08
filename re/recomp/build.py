"""Build the recompiled EBOOT + host into recomp/out/mnr.exe with llvm-mingw clang, in parallel.

Usage: python -I build.py [--regen] [--opt O1] [--jobs N]
  --regen   re-run ppu2c.py over the whole EBOOT text first
Object files are reused when newer than their source and runtime.h.
"""
import argparse
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RE = os.path.dirname(HERE)
OUT = os.path.join(HERE, 'out')
# Builds of the game's executable: the disc version and the 1.11 patch. Each has its own decrypted ELF,
# Ghidra function list, hooks, guest-address header, SPU program list and output directory.
GAMES = {
    'v100': dict(elf=os.path.join(RE, 'bin', 'EBOOT.elf'), funcs=os.path.join(RE, 'functions.tsv'), hooks='hooks_v100.txt',
                 header='game_v100.h', out=os.path.join(HERE, 'out'), text_hi='d30a28', spu=None),
    'v111': dict(elf=os.path.join(RE, 'bin_v111', 'EBOOT.elf'), funcs=os.path.join(RE, 'functions_v111.tsv'), hooks='hooks_v111.txt',
                 header='game_v111.h', out=os.path.join(HERE, 'out_v111'), text_hi='d53b78',
                 spu=['13c1e80', '13c3780', '13c4f80', '13c7180',
                      '13a9d00', '13c8a80', '13a4c00', '1396080', '13b7180', '139e980', '1386600', '13e2080', '13d7d00', '13d1a80',
                      'job:e7d700:490', 'job:e7ce00:110', 'job:e80700:1b10', 'job:e7ea00:910', 'job:e7fe80:810',
                      'job:e84700:1f10', 'job:e89e00:2610', 'job:e90780:1710', 'job:e91f00:1990', 'job:e93900:88a0', 'job:e9c200:4c10',
                      'job:ea6480:890', 'job:ea6d80:f10', 'job:ea0e80:4510']),
}
CC = os.path.join(os.path.dirname(RE), 'tools', 'llvm-mingw-20261006-ucrt-x86_64', 'bin', 'clang.exe')


def dep_mtime(path, seen=None):
    """Newest mtime of a source and the local headers it includes, recursively."""
    seen = set() if seen is None else seen
    if path in seen or not os.path.exists(path):
        return 0
    seen.add(path)
    t = os.path.getmtime(path)
    for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', open(path, errors='replace').read(), re.M):
        t = max(t, dep_mtime(os.path.join(os.path.dirname(path), m.group(1)), seen))
    return t


MARCH = 'x86-64-v2'   # one build for every PC (the ~8% a Haswell-only build gained isn't worth a second build)

PGO = []   # -fprofile-generate / -fprofile-use=... (main)
PGO_GEN_SKIP = {'recomp_041.c', 'recomp_063.c', 'recomp_065.c', 'recomp_068.c'}   # their huge functions took the instrumented -O2 compile over an hour each
def compile_one(src, opt, lto):
    obj = src[:-2] + ('.lto.o' if lto else '.o')
    if os.path.exists(obj) and os.path.getmtime(obj) > dep_mtime(src):
        return src, 0, ''
    flags = ['-flto=thin'] if lto else []
    # CPU baseline x86-64-v2: runs on any x86-64 PC from ~2009 (SSE4.2; the VMX helpers in vmx.h have SSE versions
    # of their AVX2/FMA operations); no FP contraction, so results match the Cell
    flags = flags + [f'-march={MARCH}', '-mtune=haswell', '-ffp-contract=off']
    pgo = PGO
    if PGO == ['-fprofile-generate'] and os.path.basename(src) in PGO_GEN_SKIP: pgo = []   # instrumenting these took hours
    p = subprocess.run([CC, f'-{opt}', '-g', '-w', '-fwrapv', *flags, *pgo, '-c', src, '-o', obj],
                       capture_output=True, text=True)
    return src, p.returncode, p.stderr[:600]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--regen', action='store_true')
    ap.add_argument('--opt', default='O2')
    ap.add_argument('--pgo-gen', action='store_true', help='profile-guided optimisation, step 1: an instrumented build (writes re/mnr.profraw on close)')
    ap.add_argument('--pgo-use', metavar='PROFDATA', help='step 2: build optimised with a merged profile (llvm-profdata merge -o mnr.profdata mnr.profraw)')
    ap.add_argument('--trace', action='store_true', help='fill the per-thread call ring on every guest call (crash dumps; slower)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--exe', default='mnr.exe', help='output name in out/ (link a copy while mnr.exe is running)')
    ap.add_argument('--lto', action='store_true', help='ThinLTO: cross-file inlining (objects *.lto.o)')
    ap.add_argument('--spu', action='store_true', help='recompile the SPU programs (spu2c.py) for this build')
    ap.add_argument('--game', default='v111', choices=sorted(GAMES), help='which EBOOT build: v100 (disc) or v111 (1.11 patch)')
    args = ap.parse_args()
    t0 = time.time()
    g = GAMES[args.game]
    global OUT
    OUT = g['out']
    os.makedirs(OUT, exist_ok=True)
    if args.regen or not any(f.startswith('recomp_') for f in os.listdir(OUT)):
        print('[stage] translating the game code to C', flush=True)   # ([stage]/[compile] lines: the launcher's progress)
        env = dict(os.environ, MNR_HOOKS=os.path.join(HERE, g['hooks']))
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'ppu2c.py'), g['elf'], g['funcs'], OUT, '10000', g['text_hi']],
                       check=True, env=env)
    # SPU programs compiled ahead of time: again whenever spu2c.py changes (its output and table layout change with it,
    # and the launcher's update builds don't pass --spu)
    spu_stamp = os.path.join(OUT, 'spu2c.stamp')
    with open(os.path.join(HERE, 'spu2c.py'), 'rb') as f: spu_hash = hashlib.sha256(f.read()).hexdigest()
    spu_old = open(spu_stamp).read().strip() if os.path.exists(spu_stamp) else ''
    if g['spu'] and (args.spu or spu_old != spu_hash or not os.path.exists(os.path.join(OUT, 'spu_progs.c'))):
        print('[stage] translating the SPU programs', flush=True)
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'spu2c.py'), g['elf'], OUT, *g['spu']], check=True)
        with open(spu_stamp, 'w') as f: f.write(spu_hash)
    shutil.copy2(os.path.join(HERE, g['header']), os.path.join(OUT, 'game.h'))
    global PGO
    if args.pgo_gen: PGO = ['-fprofile-generate']
    elif args.pgo_use: PGO = [f'-fprofile-use={os.path.abspath(args.pgo_use)}', '-Wno-profile-instr-out-of-date', '-Wno-profile-instr-unprofiled']
    mode = MARCH + (' pgo-gen' if args.pgo_gen else f' pgo-use {os.path.getmtime(args.pgo_use):.0f}' if args.pgo_use else '')
    stamp = os.path.join(OUT, 'march.txt')   # objects built for another CPU baseline or PGO mode are rebuilt
    if not os.path.exists(stamp) or open(stamp).read().strip() != mode:
        for f in os.listdir(OUT):
            if f.endswith('.o'):
                os.remove(os.path.join(OUT, f))
        open(stamp, 'w').write(mode)
    opts = os.path.join(OUT, 'build_opts.h')   # rewritten only when it changes (everything including runtime.h rebuilds)
    want = ('#define MNR_TRACE 1' + chr(10) if args.trace else '') + ('#define MNR_PGO_GEN 1' + chr(10) if args.pgo_gen else '') or '/* no options */' + chr(10)
    if not os.path.exists(opts) or open(opts).read() != want:
        open(opts, 'w').write(want)
    for f in ('runtime.h', 'vmx.h', 'host.h', 'host.c', 'rsx.h', 'rsx.c', 'present.h', 'present.c', 'rsx_shader.h', 'rsx_shader.c', 'rsx_render.c', 'png.h', 'png.c', 'imgenc.h', 'imgenc.c', 'input.h', 'input.c', 'net.h', 'net.c', 'p2p.h', 'p2p.c', 'native.c', 'audio.h', 'audio.c', 'rsx_hlsl.h', 'rsx_hlsl.c', 'rsx_hlsltest.c', 'rsx_tex.h', 'rsx_d3d.h', 'rsx_d3d.c', 'spu.h', 'spu.c', 'spu_core.h', 'voice.h', 'voice.c', 'voice_celp.c', 'fwextract.h', 'fwextract.c', 'fwkeys.h'):
        shutil.copy2(os.path.join(HERE, f), OUT)
    sources = sorted(os.path.join(OUT, f) for f in os.listdir(OUT) if f.endswith('.c'))
    failed = []
    print(f'[stage] compiling {len(sources)} files', flush=True)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(compile_one, s, args.opt, args.lto) for s in sources]
        for i, fu in enumerate(concurrent.futures.as_completed(futs), 1):
            src, rc, err = fu.result()
            print(f'[compile] {i}/{len(sources)} {os.path.basename(src)}', flush=True)
            if rc:
                failed.append(f'{os.path.basename(src)}: {err}')
    if failed:
        print('\n'.join(failed[:5]))
        sys.exit('compile failed')
    suffix = '.lto.o' if args.lto else '.o'
    objs = sorted(os.path.join(OUT, f) for f in os.listdir(OUT) if f.endswith(suffix) and (args.lto or not f.endswith('.lto.o')))
    print('[stage] linking (optimising across files)' if args.lto else '[stage] linking', flush=True)
    lto_link = ['-flto=thin', f'-{args.opt}', f'-Wl,--thinlto-jobs={args.jobs}'] if args.lto else []
    subprocess.run([CC, *lto_link, *(['-fprofile-generate'] if args.pgo_gen else []), '-g', '-Wl,--stack,268435456', *objs, '-lgdi32', '-luser32', '-lwinmm', '-ld3d11', '-ldinput8', '-lws2_32', '-ldxgi', '-ld3dcompiler_47', '-ldxguid', '-lsynchronization', '-lbcrypt', '-o', os.path.join(OUT, args.exe)], check=True)
    print(f'built {os.path.relpath(os.path.join(OUT, args.exe), HERE)} ({args.game}) from {len(sources)} sources in {time.time() - t0:.0f}s')


if __name__ == '__main__':
    main()
