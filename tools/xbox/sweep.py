#!/usr/bin/env python3
"""The sweep: "every N64 game" measured (docs/testing.md, docs/analyzer.md).

Runs `n64rx build <rom> --fast` over every ROM in a folder, one entry per
game, several at a time, each with a timeout, and writes where each one
stopped: analyse / recompile / compile / link / xbe, the first error line,
the time. With --run N each XBE that built is booted in xemu for N seconds
(one at a time, the caller's slot: . /c/xemu/nrx-envN.sh first) and the
last log lines are kept. With --learn R as well, the learning loop runs R
rounds: each game that learnt something ([LEARN] lines) and presents no
frames is built again with its boot logs as --learn and booted again, as
the console's loop would (docs/analyzer.md).

  python tools/xbox/sweep.py "P:/Game Console ISO/N64/Roms" [--jobs 5] [--timeout 1800]
         [--tag NAME] [--only REGEX] [--redo] [--run SECONDS] [--learn ROUNDS] [--list]

Everything it writes stays in build-host/sweep/<tag>/ (never tracked: the
logs name the user's ROMs and the XBEs hold their code): results.json,
table.md, logs/<code>.txt (n64rx's output), out/<code>/ (the XBE folder),
run/<code>.txt (the serial log's tail), learn/<code>-<n>.log (whole boot logs, --learn). A second run resumes: games
already in results.json are skipped unless --redo or --only names them.
"""
import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib import N64rxError, rom as romlib   # noqa: E402

# dump tags that mark a bad, hacked, trained, overdumped or fixed copy
BAD_TAG = re.compile(r'\[(b|h|t|o|f|p|T)\d*[^\]]*\]|\b(beta|hack|proto|demo|kiosk)\b', re.I)
REGION_RANK = {'E': 0, 'P': 1, 'X': 1, 'Y': 1, 'U': 2, 'D': 3, 'F': 3, 'J': 4}
STEPS = {'1': 'analyse', '2': 'recompile', '3': 'compile', '4': 'xbe', '5': 'xbe'}
WIN = os.name == 'nt'


def header(path):
    """(code, title) from the first bytes, any byte order; None if not a ROM"""
    with open(path, 'rb') as f:
        d = f.read(0x40)
    try:
        z, _o = romlib.normalise(d)
    except N64rxError:
        return None
    return z[0x3B:0x3F].decode('ascii', 'replace'), romlib.parse('x', z + bytes(0x100000)).title


def pick_roms(folder):
    """one ROM per game: grouped by the first three letters of the game code
    (the game without its region), the best dump wins: [!], then USA, then
    Europe, then Japan. Returns (chosen, skipped) lists of dicts."""
    groups, skipped = {}, []
    for p in sorted(pathlib.Path(folder).iterdir()):
        if not p.is_file():
            continue
        if p.suffix.lower() not in romlib.EXTENSIONS:
            skipped.append({'file': p.name, 'why': 'not a ROM extension'})
            continue
        if BAD_TAG.search(p.name):
            skipped.append({'file': p.name, 'why': 'bad/hacked/beta dump tag'})
            continue
        h = header(p)
        if h is None:
            skipped.append({'file': p.name, 'why': 'no N64 header'})
            continue
        code, title = h
        rank = (0 if '[!]' in p.name or '(!)' in p.name else 1, REGION_RANK.get(code[3:4], 5),
                0 if p.suffix.lower() == '.z64' else 1, p.name)
        groups.setdefault(code[:3], []).append((rank, {'file': p.name, 'path': str(p), 'code': code, 'title': title}))
    chosen = []
    for _k, items in sorted(groups.items()):
        items.sort(key=lambda r: r[0])
        chosen.append(items[0][1])
        skipped += [{'file': i[1]['file'], 'why': f'duplicate of {items[0][1]["file"]}'} for i in items[1:]]
    return chosen, skipped


ERR = re.compile(r'(error:|Error|error |undefined symbol|Traceback|Exception|Assertion|terminate called|'
                 r'could not|failed)', re.I)


def first_error(text, log_path):
    """the line that says why: a Python traceback's last line, the first
    compiler/linker error in n64rx.log, N64Recomp's own complaint"""
    lines = text.splitlines()
    if 'Traceback (most recent call last)' in text:
        tb = [l for l in lines if l and not l.startswith(' ')]
        return 'TRACEBACK ' + (tb[-1] if tb else '?')
    log = []
    if log_path and pathlib.Path(log_path).is_file():
        log = pathlib.Path(log_path).read_text(encoding='utf-8', errors='replace').splitlines()
    for l in log:   # a generated file clang rejected: its own log
        m = re.search(r'FAILED \(see (.+\.log)\)', l)
        if m and pathlib.Path(m.group(1)).is_file():
            for u in pathlib.Path(m.group(1)).read_text(encoding='utf-8', errors='replace').splitlines():
                if 'error:' in u:
                    return re.sub(r'^.*/gen/', 'gen/', u.strip())[:300]
    for l in log:   # clang/lld
        if re.search(r'\berror:|undefined symbol|ld\.lld: error', l):
            return l.strip()[:300]
    for l in log:   # N64Recomp
        if re.search(r'Error|error|Unhandled|unsupported|Failed|failed', l) and 'n64rx' not in l:
            return l.strip()[:300]
    for l in lines:   # n64rx's own message
        if l.startswith('n64rx:') or l.lower().startswith('error'):
            return l.strip()[:300]
    for l in lines:
        if ERR.search(l):
            return l.strip()[:300]
    return (lines[-1].strip() if lines else '')[:300]


def classify(stage, err, text):
    """a short category for the triage table"""
    e = err or ''
    if stage == 'xbe':
        return 'built'
    if e.startswith('TRACEBACK'):
        return 'n64rx traceback'
    if stage == 'timeout':
        return 'timeout'
    if stage == 'analyse':
        for pat, cat in (('boot segment would be', 'analyse: boot segment'), ('entry code', 'analyse: entry code'),
                         ('compress', 'analyse: compressed'), ('could not be analysed', 'analyse: other')):
            if pat in text:
                return cat
        return 'analyse: other'
    if stage == 'recompile':
        if 'Error recompiling' in e or 'refused' in text:
            return 'recompile: refused'
        return 'recompile: other'
    if stage in ('compile', 'link'):
        if 'undefined symbol' in e:
            return 'link: undefined symbol'
        if 'error:' in e:
            return f'{stage}: clang error'
        return f'{stage}: other'
    return stage


def learn_logs(sdir, code):
    """the serial logs the learning rounds kept for a game, oldest first"""
    return sorted((sdir / 'learn').glob(f'{code}-*.log'), key=lambda p: int(p.stem.rsplit('-', 1)[1]))


def build_one(g, args, sdir):
    code = g['code']
    out = sdir / 'out' / code
    logf = sdir / 'logs' / f'{code}.txt'
    cmd = [sys.executable, str(ROOT / 'tools/n64rx/n64rx.py'), 'build', g['path'], '--out', str(out),
           '--engine', 'native', '--fast'] + (['--analyze'] if args.analyze else [])
    for p in learn_logs(sdir, code):   # what earlier rounds learnt (--learn)
        cmd += ['--learn', str(p)]
    t0 = time.time()
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=args.timeout,
                           env=dict(os.environ, N64RX_BUILD_ROOT=str(ROOT / 'build-sweep')),
                           encoding='utf-8', errors='replace')
        text, rc, timed_out = r.stdout + r.stderr, r.returncode, False
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or '') + (e.stderr or '') if isinstance(e.stdout, str) else ''
        rc, timed_out = -1, True
    dt = time.time() - t0
    logf.write_text(text, encoding='utf-8')
    steps = re.findall(r'^\[(\d)/\d\]', text, re.M)
    stage = STEPS.get(steps[-1], 'analyse') if steps else 'analyse'
    m = re.search(r'full log: (.+n64rx\.log)', text)
    blog = m.group(1).strip() if m else None
    if blog is None:
        cand = ROOT / 'build-sweep' / re.sub(r'[^a-z0-9]', '', code.lower()) / 'n64rx.log'
        blog = str(cand) if cand.exists() else None
    if rc == 0 and 'Done:' in text:
        stage, err = 'xbe', ''
    else:
        err = first_error(text, blog if stage != 'analyse' else None)
        if stage == 'compile' and blog and re.search(r'undefined symbol|ld\.lld|lld-link', err):
            stage = 'link'
        if timed_out:
            stage, err = 'timeout', f'timed out at {stage} after {args.timeout} s'
    xbe = None
    if stage == 'xbe':
        found = list(out.glob('*/default.xbe'))
        xbe = str(found[0]) if found else None
    return {**g, 'stage': stage, 'category': classify(stage, err, text), 'error': err, 'seconds': round(dt),
            'xbe': xbe, 'blog': blog}


def write_table(sdir, results, skipped):
    rows = sorted(results.values(), key=lambda r: (r['stage'] != 'xbe', r['category'], r['title']))
    counts = {}
    for r in rows:
        counts[r['stage']] = counts.get(r['stage'], 0) + 1
    cats = {}
    for r in rows:
        cats[r['category']] = cats.get(r['category'], 0) + 1
    out = [f'# Sweep {sdir.name} ({len(rows)} games, {len(skipped)} files skipped)', '',
           '| stage | games |', '|---|---|'] + [f'| {k} | {v} |' for k, v in sorted(counts.items())]
    out += ['', '| category | games |', '|---|---|'] + [f'| {k} | {v} |' for k, v in
                                                          sorted(cats.items(), key=lambda kv: -kv[1])]
    runs = {}
    for r in rows:
        if r.get('run'):
            runs[r['run']['category']] = runs.get(r['run']['category'], 0) + 1
    if runs:
        out += ['', '| in xemu | games |', '|---|---|'] + [f'| {k} | {v} |' for k, v in
                                                          sorted(runs.items(), key=lambda kv: -kv[1])]
    gfam, afam = {}, {}
    for r in rows:
        for g in (r.get('run') or {}).get('gfx', []):
            k = g[g.rfind('(') + 1:-1] if not g.startswith('unknown') else 'unknown'
            gfam.setdefault(k, set()).add(r['code'])
        for a in (r.get('run') or {}).get('audio', []):
            afam.setdefault(a, set()).add(r['code'])
        if r.get('run') and not r['run'].get('audio'):
            afam.setdefault('none seen', set()).add(r['code'])
    if gfam:
        out += ['', '| graphics microcode | games |', '|---|---|'] + [f'| {k} | {len(v)} |' for k, v in
                                                                     sorted(gfam.items(), key=lambda kv: -len(kv[1]))]
    if afam:
        out += ['', '| audio microcode | games |', '|---|---|'] + [f'| {k} | {len(v)} |' for k, v in
                                                                  sorted(afam.items(), key=lambda kv: -len(kv[1]))]
    out += ['', '| game | code | stage | category | s | first error | graphics | audio | run |',
            '|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        e = (r['error'] or '').replace('|', '\\|')[:160]
        run = r.get('run') or {}
        out.append(f"| {r['title']} | {r['code']} | {r['stage']} | {r['category']} | {r['seconds']} | {e} | "
                   f"{'; '.join(run.get('gfx', []))} | {', '.join(run.get('audio', []))} | {run.get('summary', '')} |")
    (sdir / 'table.md').write_text('\n'.join(out) + '\n', encoding='utf-8')


RUN_KEYS = re.compile(r'\[(GFX|AUDIO|LEARN|FATAL|ULTRA|INTERP|CRASH|OS|SP|RECOMP|MEM)\]|missing|unknown', re.I)
# stop early: a crash is reported or a fatal card is up. The run's own length used to be cut at its
# 30 s heartbeat whatever --run said: on this machine's xemu (about 2 fps) Banjo-Kazooie, GoldenEye
# and Blast Corps show no frame by then and 80-280 by 90 s. Now the heartbeat at --run's seconds
RUN_STOP = r'\[CRASH\] end|\[FATAL\]'
# a boot xemu stalled before the loading screen's first frames: tried again this many times
RETRIES = int(os.environ.get('SWEEP_RETRIES', '2'))


N64_REGIONS = [(0x00000000, 0x80000000, 'KUSEG (TLB-mapped)'), (0x80000000, 0x80800000, 'KSEG0 RDRAM'),
               (0x80800000, 0xA0000000, 'KSEG0 past RDRAM'), (0xA0000000, 0xA0800000, 'KSEG1 RDRAM'),
               (0xA3F00000, 0xA4000000, 'KSEG1 RDRAM registers'), (0xA4000000, 0xA4100000, 'KSEG1 SP memory/registers'),
               (0xA4100000, 0xA4900000, 'KSEG1 RCP registers'), (0xA5000000, 0xA6000000, 'KSEG1 64DD'),
               (0xA8000000, 0xB0000000, 'KSEG1 cart domain 2'), (0xB0000000, 0xBFC00000, 'KSEG1 cart ROM'),
               (0xBFC00000, 0xC0000000, 'KSEG1 PIF'), (0xC0000000, 0x100000000, 'KSSEG/KSEG3 (TLB-mapped)')]


def run_summary(lines):
    """what a boot reached, from the serial log (xhw_main.c's [BEAT], [CRASH],
    [FATAL]; learn.c's [LEARN]; more3.c's [ULTRA] missing; ucode.c's [GFX])"""
    presented = retrace = 0
    for l in lines:
        m = re.search(r'\[BEAT\] \d+s: retrace (\d+), presented (\d+)', l)
        if m:
            retrace, presented = max(retrace, int(m.group(1))), max(presented, int(m.group(2)))
    fatal = next((l.strip() for l in lines if l.startswith('[FATAL]')), '')
    crash = next((l.strip()[8:] for l in lines if re.match(r'\[CRASH\] (read|write|execute) ', l)), '')
    if not crash:
        crash = next((l.strip()[8:] for l in lines if l.startswith('[CRASH]')), '')
    missing = sorted({m.group(1) for l in lines for m in [re.search(r'\[ULTRA\] missing (\S+)', l)] if m})
    learn = sum(1 for l in lines if '[LEARN]' in l)
    ucode = next((l.strip()[6:] for l in lines if l.startswith('[GFX] ucode')), '')
    gfx, audio = microcodes(lines)
    if fatal:
        cat = 'fatal: ' + re.sub(r'[0-9a-fA-F]{8}', 'X', fatal[8:])[:60]
    elif crash:
        m = re.search(r'(read|write|execute) of address ([0-9a-f]{8})', crash)
        base = next((int(b.group(1), 16) for l in lines for b in [re.search(r'\[MEM\] rdram: \d+ KB at ([0-9a-f]{8})', l)]
                     if b), None)
        if base is None:   # boot.c's window line: the physical base, mapped at 0x80000000 + it
            base = next((0x80000000 | int(b.group(1), 16) for l in lines
                         for b in [re.search(r'\[RECOMP\] window: .*rdram base ([0-9a-f]{8})', l)] if b), None)
        cat = 'crash'
        if m and base is not None:   # the host address back to the N64's: what the game touched
            n64 = (int(m.group(2), 16) - base + 0x80000000) & 0xFFFFFFFF
            where = next((w for lo, hi, w in N64_REGIONS if lo <= n64 < hi), 'elsewhere')
            cat = f'crash: {m.group(1)} {where}'
            crash += f' (N64 {n64:08x})'
        elif m:
            cat = f'crash: {m.group(1)} host {m.group(2)[:2]}xxxxxx'
    elif not lines:
        cat = 'no log'
    elif presented:
        cat = 'frames'
    else:
        cat = 'no frames'
    summary = cat + (f' ({crash[:50]})' if crash and not fatal else '') + f', presented {presented}' \
        + (f', {learn} LEARN' if learn else '') + (f', missing {" ".join(missing)[:60]}' if missing else '') \
        + (f', ucode {ucode[:40]}' if ucode else '')
    return {'category': cat, 'presented': presented, 'retrace': retrace, 'fatal': fatal, 'crash': crash,
            'missing': missing, 'learn': learn, 'ucode': ucode, 'gfx': gfx, 'audio': audio,
            'summary': summary.replace('|', '/')}


GFX_UC = re.compile(r'^\[GFX\] ucode (.*?) \(data [0-9a-f]+\): (.*)$')
GFX_NOTEXT = re.compile(r'^\[GFX\] ucode at [0-9a-f]+ has no version text: its display lists read as (.*)$')
AUD_UC = re.compile(r'^\[AUDIO\] microcode [0-9a-f]+ .*\): (\S+(?: \(ABI\d\))?) \(')


def microcodes(lines):
    """the graphics microcodes a boot switched to (version text: family) and
    the audio ABIs the HLE took (docs/roadmap.md B3: count them before
    choosing which to write); 'unknown' for a graphics one with no version
    text"""
    gfx, audio = [], []
    for l in lines:
        l = l.strip()
        m = GFX_UC.match(l)
        if m:
            v = re.sub(r'\s+', ' ', m.group(1)).replace('RSP Gfx ucode ', '').replace('RSP SW Version: ', 'Fast3D ')
            e = f'{v} ({m.group(2).split(",")[0]})'
        elif GFX_NOTEXT.match(l):
            e = f'unknown (read as {GFX_NOTEXT.match(l).group(1)})'
        else:
            m = AUD_UC.match(l)
            if m and m.group(1) not in audio:
                audio.append(m.group(1))
            continue
        if e not in gfx:
            gfx.append(e)
    return gfx, audio


def slot_env(slot):
    """xemu slot N's run folder and config (/c/xemu/nrx-envN.sh's); {} = the caller's"""
    if slot is None:
        return {}
    base = '/c/xemu' if WIN else str(pathlib.Path.home() / 'xemu')
    cfg = 'C:/xemu' if WIN else base
    return {'MX_RUN': f'{base}/nrx-run{slot}', 'MX_XEMU_ARGS': f'-config_path {cfg}/slots/{slot}/xemu.toml'}


def run_one(r, secs, sdir, keep_as=None, slot=None):
    """boot the XBE in xemu (the caller's slot, or --slots' one) and keep the serial log's
    interesting lines and its tail (and the whole log as learn/<keep_as>.log,
    for the next learning round)"""
    env = dict(os.environ, MX_XBE=r['xbe'], MX_SHOTS='0', **slot_env(slot))
    bash = 'C:/Program Files/Git/bin/bash.exe' if WIN else 'bash'
    t0 = time.time()
    run_dir = env.get('MX_RUN', str(pathlib.Path.home() / 'xemu/nrx-run'))
    if WIN and re.match(r'^/[a-z]/', run_dir):
        run_dir = run_dir[1].upper() + ':' + run_dir[2:]
    log = pathlib.Path(run_dir) / 'serial.log'
    err = ''
    lines = []
    for _try in range(1 + RETRIES):
        # the last boot's log must never stand for this one: s15 read one game's log for 165 others once
        # xemu_run.sh began failing before it truncated the log
        try:
            log.unlink()
        except FileNotFoundError:
            pass
        except OSError as e:   # held open by a stray xemu: this boot could not be told from the last
            err = f'serial.log in use: {e}'
            break
        try:
            p = subprocess.run([bash, str(ROOT / 'tools/xbox/xemu_run.sh'), str(secs), RUN_STOP + rf'|\[BEAT\] {secs}s'], cwd=ROOT, env=env,
                               capture_output=True, text=True, timeout=secs + 180)
            if p.returncode:
                tail = (p.stderr or p.stdout or '').strip().splitlines()
                err = f"xemu_run.sh: {tail[-1][:200] if tail else f'exit {p.returncode}'}"
        except subprocess.TimeoutExpired:
            pass
        lines = log.read_text(encoding='utf-8', errors='replace').replace('\r', '').splitlines() if log.is_file() else []
        # xemu here stalls some boots at the loading screen before its first 30 frames (the game never
        # starts): booted again, not counted as "no frames"
        if any(l.startswith('[BOOT] loading screen:') for l in lines):
            break
    keep = [l for l in lines if RUN_KEYS.search(l)][:120]
    # every microcode line too, past the 120 (the audio set-up and [LEARN] lines crowded them out: --rescan's
    # graphics and audio columns came out empty for games that switch microcodes late)
    keep += [l for l in lines if (l.startswith('[GFX] ucode') or l.startswith('[AUDIO] microcode')) and l not in keep]
    keep += ['--- tail ---'] + lines[-40:]
    (sdir / 'run' / f"{r['code']}.txt").write_text('\n'.join(keep) + '\n', encoding='utf-8')
    if keep_as and log.is_file():
        shutil.copyfile(log, sdir / 'learn' / f'{keep_as}.log')
    out = {**run_summary(lines), 'lines': len(lines), 'seconds': round(time.time() - t0)}
    if not lines and err:
        out['summary'], out['error'] = f'no log ({err[:80]})'.replace('|', '/'), err
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder')
    ap.add_argument('--jobs', type=int, default=5)
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per ROM build')
    ap.add_argument('--tag', default='latest', help='build-host/sweep/<tag>/')
    ap.add_argument('--only', help='regex on the file name or game code')
    ap.add_argument('--redo', action='store_true', help='build games already in results.json again')
    ap.add_argument('--analyze', action='store_true', help='--analyze even where a pack matches')
    ap.add_argument('--run', type=int, default=0, metavar='SECONDS', help='boot each XBE that built in xemu')
    ap.add_argument('--learn', type=int, default=0, metavar='ROUNDS',
                    help='with --run: the learning loop, ROUNDS times: each game that learnt something and '
                         'presents no frames is built again with its boot logs (--learn) and booted again')
    ap.add_argument('--learn-all', action='store_true', help='--learn for the games that present frames too')
    ap.add_argument('--slots', help='with --run: boot on these xemu slots at once (e.g. 10,11,12: '
                    'nrx-runN and slots/N/xemu.toml); each boot runs slower with several')
    ap.add_argument('--list', action='store_true', help='only show which ROM stands for each game')
    ap.add_argument('--rescan', action='store_true', help='redo the error lines and categories from the logs')
    a = ap.parse_args()
    if 'N64RX_RECOMP' not in os.environ:
        print('note: N64RX_RECOMP is not set; each worktree builds its own N64Recomp', flush=True)
    sdir = ROOT / 'build-host/sweep' / a.tag
    for d in ('logs', 'out', 'run', 'learn'):
        (sdir / d).mkdir(parents=True, exist_ok=True)
    chosen, skipped = pick_roms(a.folder)
    (sdir / 'skipped.json').write_text(json.dumps(skipped, indent=1), encoding='utf-8')
    if a.list:
        for g in chosen:
            print(f"{g['code']}  {g['file']}")
        print(f'{len(chosen)} games, {len(skipped)} files skipped')
        return 0
    rpath = sdir / 'results.json'
    results = json.loads(rpath.read_text(encoding='utf-8')) if rpath.is_file() else {}
    only = re.compile(a.only, re.I) if a.only else None
    if a.rescan:
        for r in results.values():
            rf = sdir / 'run' / f"{r['code']}.txt"
            if r.get('run') and rf.is_file():   # what the boot kept (run_one's lines)
                r['run'].update(run_summary(rf.read_text(encoding='utf-8', errors='replace').splitlines()))
            if r['stage'] in ('xbe', 'timeout', 'sweep'):
                continue
            text = (sdir / 'logs' / f"{r['code']}.txt").read_text(encoding='utf-8', errors='replace')
            r['error'] = first_error(text, r.get('blog') if r['stage'] != 'analyse' else None)
            r['category'] = classify(r['stage'], r['error'], text)
        rpath.write_text(json.dumps(results, indent=1), encoding='utf-8')
        write_table(sdir, results, skipped)
        print(f'table: {sdir / "table.md"}')
        return 0
    todo = [g for g in chosen if (only.search(g['file']) or only.search(g['code'])) if only] if only else \
        [g for g in chosen if a.redo or g['code'] not in results]
    lock = threading.Lock()
    print(f'{len(chosen)} games ({len(skipped)} files skipped), {len(todo)} to build, {a.jobs} at a time', flush=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(build_one, g, a, sdir): g for g in todo}
        for i, f in enumerate(cf.as_completed(futs), 1):
            g = futs[f]
            try:
                r = f.result()
            except Exception as e:   # the sweep itself must not stop
                r = {**g, 'stage': 'sweep', 'category': 'sweep error', 'error': repr(e)[:300], 'seconds': 0,
                     'xbe': None, 'blog': None}
            with lock:
                old = results.get(g['code'], {})
                if old.get('run') and r['stage'] == 'xbe':
                    r['run'] = old['run']
                results[g['code']] = r
                rpath.write_text(json.dumps(results, indent=1), encoding='utf-8')
                write_table(sdir, results, skipped)
            print(f"[{i}/{len(todo)}] {r['stage']:9} {r['seconds']:5}s {g['code']} {g['file'][:40]:40} "
                  f"{r['error'][:90]}", flush=True)
    if a.run:
        built = [r for r in results.values() if r['stage'] == 'xbe' and r.get('xbe')
                 and (not only or only.search(r['file']) or only.search(r['code']))]
        todo = [r for r in built if a.redo or only or not r.get('run')]
        slots = queue.Queue()
        for s in (a.slots.split(',') if a.slots else [None]):
            slots.put(s)

        def boot(r):   # each boot takes a free slot and gives it back
            s = slots.get()
            try:
                return run_one(r, a.run, sdir, keep_as=f"{r['code']}-{len(learn_logs(sdir, r['code'])) + 1}"
                               if a.learn else None, slot=s)
            finally:
                slots.put(s)
        with cf.ThreadPoolExecutor(slots.qsize()) as ex:
            futs = {ex.submit(boot, r): r for r in todo}
            for i, f in enumerate(cf.as_completed(futs), 1):
                r = futs[f]
                r['run'] = f.result()
                with lock:
                    rpath.write_text(json.dumps(results, indent=1), encoding='utf-8')
                    write_table(sdir, results, skipped)
                print(f"[run {i}/{len(todo)}] {r['code']} {r['file'][:40]:40} {r['run']['summary']}", flush=True)
    for rnd in range(1, a.learn + 1 if a.run else 1):
        # the learning loop (docs/analyzer.md): what a boot learnt goes into the next build
        todo = [r for r in results.values() if r['stage'] == 'xbe' and r.get('run') and r['run']['learn']
                and (a.learn_all or r['run']['category'] != 'frames')
                and (not only or only.search(r['file']) or only.search(r['code']))]
        print(f'learning round {rnd}: {len(todo)} games built again with what they learnt', flush=True)
        if not todo:
            break
        for r in todo:   # a run from before --learn kept no log: boot it once more to have one
            if not learn_logs(sdir, r['code']):
                r['run'] = run_one(r, a.run, sdir, keep_as=f"{r['code']}-1")
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            futs = {ex.submit(build_one, r, a, sdir): r for r in todo}
            for f in cf.as_completed(futs):
                old = futs[f]
                try:
                    b = f.result()
                except Exception as e:
                    b = {**old, 'stage': 'sweep', 'category': 'sweep error', 'error': repr(e)[:300]}
                with lock:
                    b['run'], b['rounds'] = old['run'], old.get('rounds', []) + [old['run']['summary']]
                    results[old['code']] = b
        for i, r in enumerate([results[t['code']] for t in todo], 1):
            if r['stage'] != 'xbe':
                print(f"[learn {rnd} {i}/{len(todo)}] {r['code']} build failed: {r['error'][:90]}", flush=True)
                continue
            r['run'] = run_one(r, a.run, sdir, keep_as=f"{r['code']}-{len(learn_logs(sdir, r['code'])) + 1}")
            rpath.write_text(json.dumps(results, indent=1), encoding='utf-8')
            write_table(sdir, results, skipped)
            print(f"[learn {rnd} {i}/{len(todo)}] {r['code']} {r['file'][:40]:40} {r['run']['summary']}", flush=True)
    write_table(sdir, results, skipped)
    print(f'table: {sdir / "table.md"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
