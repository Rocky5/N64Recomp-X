"""n64rx's command line (PLAN.md 5.1).

  n64rx build <rom> [--out DIR] [--engine docker|native|auto] [--video auto|480|720] [--fast] [--rom-beside]
  n64rx build <rom> --iso FILE   (a disc image instead of the folder)
  n64rx info <rom> [--json]
  n64rx doctor [--install]
  n64rx games
  n64rx deploy <folder> --ftp HOST [--user xbox --pass xbox] [--to /E/Games]
  n64rx xiso <folder> [--out FILE]
  n64rx log [--pull HOST]
  n64rx clean [<pack id>]

Dropping a ROM onto n64rx (argv[1] is a ROM file) is `build`. `--events
json` before the command (the app's way in) prints JSON lines instead:
step, note, done, log, result and error events (docs/app.md)."""
import argparse
import ftplib
import io
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import time

from . import ROOT, VERSION, N64rxError, compat, doctor, engines, packs, pipeline, rom as romlib

EVENTS = False   # --events json: every line out is a JSON object (the app reads them)


def emit(ev):
    print(json.dumps(ev, ensure_ascii=False), flush=True)


def say(text):
    """a line for the user: as it is, or a log event"""
    if EVENTS:
        emit({'ev': 'log', 'text': text})
    else:
        print(text, flush=True)


def cmd_build(a):
    for f in a.learn or ():
        if not pathlib.Path(f).is_file():
            raise N64rxError(f'No learn file at {f}.', r'Copy E:\UDATA\<title id>\learn.txt off the Xbox (n64rx log '
                             '--pull <ip> fetches the logs), or pass the serial log of the run.')
    if not a.iso:
        pipeline.build(a.rom, out=a.out, engine=a.engine, video=a.video, fast=a.fast, gpr=a.gpr, analyze=a.analyze,
                       learn=tuple(a.learn or ()), rom_beside=a.rom_beside, echo=say, events=emit if EVENTS else None)
        return 0
    # --iso: the folder is built in n64rx's own place, made a disc image, then removed (the app's "ISO")
    iso = pathlib.Path(a.iso).resolve()
    if iso.suffix.lower() != '.iso':
        raise N64rxError(f'{iso.name} does not end in .iso.', 'Give the disc image a name ending in .iso.')
    staging = pipeline.cache_dir() / 'iso-staging'
    shutil.rmtree(staging, ignore_errors=True)
    result = {}

    def events(ev):   # six steps, not five; the build's result is held for the disc image's
        if ev['ev'] == 'result':
            result.update(ev)
            return
        emit({**ev, 'of': 6} if ev['ev'] == 'step' else ev)

    echo = say if EVENTS else (lambda t: None if t.lstrip().startswith(('Done:', 'Copy that folder')) else say(t))
    try:
        pipeline.build(a.rom, out=staging, engine=a.engine, video=a.video, fast=a.fast, gpr=a.gpr,
                       analyze=a.analyze, learn=tuple(a.learn or ()), rom_beside=a.rom_beside, echo=echo,
                       events=events if EVENTS else None)
        folder = next(p for p in staging.iterdir() if (p / 'default.xbe').exists())
        if EVENTS:
            emit({'ev': 'step', 'n': 6, 'of': 6, 'text': 'Making the disc image (ISO)'})
        else:
            say('[6/6] Making the disc image (ISO)')
        t = time.monotonic()
        make_iso(folder, iso)
        if EVENTS:
            emit({'ev': 'done', 'secs': round(time.monotonic() - t, 1)})
            emit({'ev': 'result', 'file': str(iso), 'status': result.get('status', '')})
        else:
            say(f'\nDone: {iso}')
            say('Burn it to a DVD-R, or put it on the console for a disc loader.')
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return 0


def estimate(rom):
    """how long a first build takes (docs/app.md: SM64 38 s, OoT 41 s on 8 cores; the ROM's size barely
    matters, the analysis is one core and the compile all of them); the cache makes a rebuild seconds"""
    return 'about 1 to 3 minutes'


def rom_info(path):
    """what the app shows when a ROM is loaded: the header, its pack, its tier and known problems (compat.py)"""
    rom = romlib.read(path)
    try:
        pack = packs.match(rom, packs.all_packs())
    except N64rxError:
        pack = None
    return {
        'rom': {'path': str(rom.path), 'title': rom.title, 'code': rom.code, 'region': rom.region,
                'revision': f'1.{rom.version}', 'size': rom.size, 'order': rom.order, 'sha1': rom.sha1,
                'cic': rom.cic},
        'pack': {'id': pack.id, 'title': pack.title} if pack else None,
        'compat': compat.report(rom.code, rom.version),
        'estimate': estimate(rom),
    }


def cmd_info(a):
    i = rom_info(a.rom)
    if a.json or EVENTS:
        print(json.dumps(i, ensure_ascii=False), flush=True)
        return 0
    r, c = i['rom'], i['compat']
    print(f'{c["name"] or r["title"]}: {r["code"]}, {r["region"]}, v{r["revision"]}, {r["size"] >> 20} MB, '
          f'{r["order"]} byte order')
    print(f'SHA-1 {r["sha1"]}' + (f', game pack {i["pack"]["id"]}' if i['pack'] else ', no game pack: built from the ROM alone'))
    print(f'Status: {c["tier"]}, {c["meaning"]}.')
    for issue in c['issues']:
        print(f'  - {issue}')
    if c['other_version']:
        print(c['other_version'])
    print(f'A first build takes {i["estimate"]}.')
    return 0


def cmd_doctor(a):
    if a.install:
        return doctor.install()
    n, d, b = doctor.report()
    return 0 if b and (n or d) else 1


def cmd_games(a):
    ps = packs.all_packs()
    if not ps:
        print('No game packs found in games/.')
        return 1
    print(f'{"pack":10} {"game":28} {"code":5} {"stage":9} title ID')
    for p in ps:
        print(f'{p.id:10} {p.title[:28]:28} {p.code:5} {p.stage:9} {p.title_id_hex}')
    print('\nn64rx build <your ROM> picks the pack by the ROM\'s SHA-1.')
    return 0


def _ftp(host, user, pw):
    try:
        f = ftplib.FTP(host, timeout=20)
        f.login(user, pw)
        return f
    except (OSError, ftplib.Error) as e:
        raise N64rxError(f'Can\'t reach the Xbox at {host} by FTP ({e}).',
                         'Check the console is on with an FTP dashboard (UnleashX, XBMC4Gamers...) and the address.')


def _title_id(folder):
    for p in packs.all_packs():
        if pipeline.folder_name(p.title) == folder.name:
            return p.title_id_hex
    return None


def rom_inside(xbe):
    """the XBE holds the ROM as its $ROM section (PLAN.md 3.5)"""
    with open(xbe, 'rb') as f:
        d = f.read(0x1000)
    try:
        base, nsec, tab = struct.unpack_from('<I', d, 0x104)[0], *struct.unpack_from('<II', d, 0x11C)
        names = [struct.unpack_from('<I', d, tab - base + i * 0x38 + 20)[0] - base for i in range(nsec)]
        return any(d[n:n + 5] == b'$ROM\0' for n in names)
    except struct.error:
        return False


def cmd_deploy(a):
    folder = pathlib.Path(a.folder)
    if not (folder / 'default.xbe').exists():
        raise N64rxError(f'{folder} has no default.xbe.', 'Give the folder n64rx build made (out/<game>).')
    # one file holds the game and the ROM; a --rom-beside build needs its ROM file too
    if not rom_inside(folder / 'default.xbe') and not any(
            p.suffix.lower() in ('.z64', '.n64', '.v64', '.rom', '.bin') for p in folder.iterdir()):
        raise N64rxError(f'{folder} has no ROM: not inside default.xbe and no ROM file next to it.',
                         'Build it again with n64rx build <your ROM> (the ROM goes inside default.xbe).')
    f = _ftp(a.ftp, a.user, a.password)
    tid = _title_id(folder)
    if tid:   # logs first: the next boot overwrites them
        logs = folder / 'logs'
        logs.mkdir(exist_ok=True)
        for name in ('boot.log', 'boot_prev.log', 'crash.log', 'hang.log', 'lastexit.txt'):
            try:
                with open(logs / name, 'wb') as out:
                    f.retrbinary(f'RETR /E/UDATA/{tid}/{name}', out.write)
            except ftplib.Error:
                (logs / name).unlink(missing_ok=True)
    dest = f'{a.to.rstrip("/")}/{folder.name}'
    for part in dest.strip('/').split('/'):
        try:
            f.mkd(part)
        except ftplib.Error:
            pass
        f.cwd(part)
    for p in sorted(folder.iterdir()):
        if p.is_file():
            with open(p, 'rb') as src:
                f.storbinary(f'STOR {p.name}', src)
            say(f'  {p.name}')
    f.quit()
    if EVENTS:
        emit({'ev': 'result', 'dest': f'{a.ftp}:{dest}'})
    else:
        print(f'Deployed to {a.ftp}:{dest}')
    return 0


def find_extract_xiso():
    for c in (shutil.which('extract-xiso'), engines.Native().nxdk / 'tools/extract-xiso/build' / f'extract-xiso{engines.EXE}'):
        if c and pathlib.Path(c).exists():
            return pathlib.Path(c)
    raise N64rxError('extract-xiso was not found.', 'It is built with nxdk (tools/xbox/setup.sh), or install it '
                     'from https://github.com/XboxDev/extract-xiso.')


def make_iso(folder, out):
    """an XISO of the folder (its files at the disc's root), by extract-xiso"""
    out.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(find_extract_xiso()), '-c', str(folder), str(out)],
                       **({'capture_output': True, 'text': True, 'errors': 'replace'} if EVENTS else {}))
    if EVENTS:
        for line in (r.stdout + r.stderr).splitlines():
            say(line)
    if r.returncode or not out.exists():
        raise N64rxError('extract-xiso failed.', 'See its output above.')


def cmd_xiso(a):
    folder = pathlib.Path(a.folder).resolve()
    out = pathlib.Path(a.out or folder.with_suffix('.iso')).resolve()
    make_iso(folder, out)
    if EVENTS:
        emit({'ev': 'result', 'file': str(out)})
    else:
        print(f'Wrote {out}: burn it to a DVD-R, or put it on the console for a disc loader.')
    return 0


def cmd_log(a):
    last = pipeline.cache_dir() / 'last.json'
    if not last.exists():
        raise N64rxError('No build has run yet.', 'n64rx build <your ROM>')
    info = json.loads(last.read_text())
    if not a.pull:
        print(pathlib.Path(info['log']).read_text(encoding='utf-8', errors='replace'))
        return 0
    pack = packs.load(packs.GAMES / info['pack'])
    f = _ftp(a.pull, a.user, a.password)
    mp = pathlib.Path(info['folder']) / 'debug/n64recomp_x.map'
    for name in ('boot.log', 'crash.log', 'learn.txt'):
        buf = io.BytesIO()
        try:
            f.retrbinary(f'RETR /E/UDATA/{pack.title_id_hex}/{name}', buf.write)
        except ftplib.Error:
            continue
        text = buf.getvalue().decode('utf-8', 'replace')
        print(f'--- {name} ---\n{text}')
        if name == 'learn.txt':   # the safety net's log (PLAN.md 3.14): the next build's --learn
            saved = pathlib.Path(info['folder']) / 'debug/learn.txt'
            saved.parent.mkdir(parents=True, exist_ok=True)
            saved.write_text(text, encoding='utf-8')
            print(f'saved as {saved}: n64rx build <rom> --learn "{saved}" recompiles that code')
        if name == 'crash.log' and mp.exists():
            saved = pathlib.Path(info['folder']) / 'debug/crash.log'
            saved.write_text(text, encoding='utf-8')
            print('--- crash.log, symbolised ---')
            subprocess.run([sys.executable, str(ROOT / 'tools/xbox/sym.py'), '--map', str(mp), str(saved)])
    f.quit()
    return 0


def cmd_clean(a):
    base = (ROOT / 'build-xbox').resolve()
    target = (base / a.pack).resolve() if a.pack else base
    if target != base and base not in target.parents:   # "clean .." was the checkout (review 3.10)
        print(f'n64rx clean: {a.pack} is not a build folder under {base}')
        return 1
    if target.exists():
        shutil.rmtree(target)
        print(f'removed {target}')
    return 0


def parser():
    ap = argparse.ArgumentParser(prog='n64rx', description='Build N64 games for the original Xbox from your own ROM.')
    ap.add_argument('--version', action='version', version=f'n64rx {VERSION}')
    ap.add_argument('--events', choices=('text', 'json'), default='text', help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('build', help='build a game from your ROM')
    b.add_argument('rom')
    b.add_argument('--out', help='where the folder for the Xbox goes (default ./out)')
    b.add_argument('--engine', choices=('auto', 'docker', 'native'), default='auto')
    b.add_argument('--video', choices=('auto', '480', '720'), default='auto', help='the default video mode')
    b.add_argument('--fast', action='store_true', help='-O1, no LTO: a quicker first look')
    b.add_argument('--analyze', action='store_true', help='analyse the ROM even when a game pack knows it (PLAN.md 3.14)')
    b.add_argument('--rom-beside', action='store_true',
                   help='the ROM as a file next to default.xbe instead of inside it (PLAN.md 3.5)')
    b.add_argument('--learn', action='append', metavar='FILE',
                   help='learn.txt from the console (or its serial log): recompile the code the safety net ran '
                        '(PLAN.md 3.14); implies --analyze')
    b.add_argument('--iso', metavar='FILE', help='a disc image (.iso) instead of the folder: for a DVD-R or a disc loader')
    b.add_argument('--gpr', choices=('32', '64'), help=argparse.SUPPRESS)
    b.set_defaults(fn=cmd_build)
    i = sub.add_parser('info', help='what a ROM is: the game, its status and known problems')
    i.add_argument('rom')
    i.add_argument('--json', action='store_true', help='one JSON object (what the app reads)')
    i.set_defaults(fn=cmd_info)
    d = sub.add_parser('doctor', help='what is installed, what is missing')
    d.add_argument('--install', action='store_true', help='install the native toolchain (tools/xbox/setup.sh)')
    d.set_defaults(fn=cmd_doctor)
    sub.add_parser('games', help='the game packs and their status').set_defaults(fn=cmd_games)
    dp = sub.add_parser('deploy', help='copy a built folder to the Xbox by FTP')
    dp.add_argument('folder')
    dp.add_argument('--ftp', required=True, metavar='HOST')
    dp.add_argument('--user', default='xbox')
    # the app passes it in N64RX_FTP_PASS: not on a command line or in a log
    dp.add_argument('--pass', dest='password', default=os.environ.get('N64RX_FTP_PASS', 'xbox'))
    dp.add_argument('--to', default='/E/Games')
    dp.set_defaults(fn=cmd_deploy)
    x = sub.add_parser('xiso', help='a DVD-R image of a built folder')
    x.add_argument('folder')
    x.add_argument('--out')
    x.set_defaults(fn=cmd_xiso)
    lg = sub.add_parser('log', help='the last build\'s log, or the console\'s logs')
    lg.add_argument('--pull', metavar='HOST')
    lg.add_argument('--user', default='xbox')
    lg.add_argument('--pass', dest='password', default=os.environ.get('N64RX_FTP_PASS', 'xbox'))
    lg.set_defaults(fn=cmd_log)
    c = sub.add_parser('clean', help='remove build folders')
    c.add_argument('pack', nargs='?')
    c.set_defaults(fn=cmd_clean)
    return ap


def main(argv=None):
    for s in (sys.stdout, sys.stderr):   # a console or pipe in a legacy code page must not stop a build
        if hasattr(s, 'reconfigure'):
            s.reconfigure(errors='replace')
    argv = list(sys.argv[1:] if argv is None else argv)
    # a ROM dropped onto n64rx.exe
    if argv and pathlib.Path(argv[0]).suffix.lower() in romlib.EXTENSIONS and pathlib.Path(argv[0]).is_file():
        argv.insert(0, 'build')
    ap = parser()
    a = ap.parse_args(argv)
    if not getattr(a, 'fn', None):
        ap.print_help()
        return 0
    global EVENTS
    EVENTS = a.events == 'json'
    try:
        return a.fn(a)
    except N64rxError as e:
        if EVENTS or getattr(a, 'json', False):
            emit({'ev': 'error', 'message': e.message, 'hint': e.hint})
            return 2
        print(f'\nn64rx: {e.message}', file=sys.stderr)
        if e.hint:
            print(f'What to do: {e.hint}', file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('\nn64rx: stopped.', file=sys.stderr)
        return 130
    except Exception as e:   # the app gets an error it can show, not a traceback it cannot read
        if not EVENTS:
            raise
        import traceback
        emit({'ev': 'log', 'text': traceback.format_exc()})
        emit({'ev': 'error', 'message': f'n64rx stopped on an internal error: {e}',
              'hint': 'This is a bug in n64rx, not in your ROM: copy the log and report it on GitHub.'})
        return 3
