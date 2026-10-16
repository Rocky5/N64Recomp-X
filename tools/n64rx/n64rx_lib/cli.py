"""n64rx's command line (PLAN.md 5.1).

  n64rx build <rom> [--out DIR] [--engine docker|native|auto] [--video auto|480|720] [--fast] [--rom-beside]
  n64rx doctor [--install]
  n64rx games
  n64rx deploy <folder> --ftp HOST [--user xbox --pass xbox] [--to /E/Games]
  n64rx xiso <folder> [--out FILE]
  n64rx log [--pull HOST]
  n64rx clean [<pack id>]

Dropping a ROM onto n64rx (argv[1] is a ROM file) is `build`."""
import argparse
import ftplib
import io
import json
import pathlib
import shutil
import struct
import subprocess
import sys

from . import ROOT, VERSION, N64rxError, doctor, engines, packs, pipeline, rom as romlib


def cmd_build(a):
    pipeline.build(a.rom, out=a.out, engine=a.engine, video=a.video, fast=a.fast, gpr=a.gpr, analyze=a.analyze,
                   rom_beside=a.rom_beside)
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
            print(f'  {p.name}')
    f.quit()
    print(f'Deployed to {a.ftp}:{dest}')
    return 0


def find_extract_xiso():
    for c in (shutil.which('extract-xiso'), engines.Native().nxdk / 'tools/extract-xiso/build' / f'extract-xiso{engines.EXE}'):
        if c and pathlib.Path(c).exists():
            return pathlib.Path(c)
    raise N64rxError('extract-xiso was not found.', 'It is built with nxdk (tools/xbox/setup.sh), or install it '
                     'from https://github.com/XboxDev/extract-xiso.')


def cmd_xiso(a):
    folder = pathlib.Path(a.folder).resolve()
    out = pathlib.Path(a.out or folder.with_suffix('.iso')).resolve()
    r = subprocess.run([str(find_extract_xiso()), '-c', str(folder), str(out)])
    if r.returncode:
        raise N64rxError('extract-xiso failed.', 'See its output above.')
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
    for name in ('boot.log', 'crash.log'):
        buf = io.BytesIO()
        try:
            f.retrbinary(f'RETR /E/UDATA/{pack.title_id_hex}/{name}', buf.write)
        except ftplib.Error:
            continue
        text = buf.getvalue().decode('utf-8', 'replace')
        print(f'--- {name} ---\n{text}')
        if name == 'crash.log' and mp.exists():
            saved = pathlib.Path(info['folder']) / 'debug/crash.log'
            saved.write_text(text, encoding='utf-8')
            print('--- crash.log, symbolised ---')
            subprocess.run([sys.executable, str(ROOT / 'tools/xbox/sym.py'), '--map', str(mp), str(saved)])
    f.quit()
    return 0


def cmd_clean(a):
    target = ROOT / 'build-xbox' / a.pack if a.pack else ROOT / 'build-xbox'
    if target.exists():
        shutil.rmtree(target)
        print(f'removed {target}')
    return 0


def parser():
    ap = argparse.ArgumentParser(prog='n64rx', description='Build N64 games for the original Xbox from your own ROM.')
    ap.add_argument('--version', action='version', version=f'n64rx {VERSION}')
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
    b.add_argument('--gpr', choices=('32', '64'), help=argparse.SUPPRESS)
    b.set_defaults(fn=cmd_build)
    d = sub.add_parser('doctor', help='what is installed, what is missing')
    d.add_argument('--install', action='store_true', help='install the native toolchain (tools/xbox/setup.sh)')
    d.set_defaults(fn=cmd_doctor)
    sub.add_parser('games', help='the game packs and their status').set_defaults(fn=cmd_games)
    dp = sub.add_parser('deploy', help='copy a built folder to the Xbox by FTP')
    dp.add_argument('folder')
    dp.add_argument('--ftp', required=True, metavar='HOST')
    dp.add_argument('--user', default='xbox')
    dp.add_argument('--pass', dest='password', default='xbox')
    dp.add_argument('--to', default='/E/Games')
    dp.set_defaults(fn=cmd_deploy)
    x = sub.add_parser('xiso', help='a DVD-R image of a built folder')
    x.add_argument('folder')
    x.add_argument('--out')
    x.set_defaults(fn=cmd_xiso)
    lg = sub.add_parser('log', help='the last build\'s log, or the console\'s logs')
    lg.add_argument('--pull', metavar='HOST')
    lg.add_argument('--user', default='xbox')
    lg.add_argument('--pass', dest='password', default='xbox')
    lg.set_defaults(fn=cmd_log)
    c = sub.add_parser('clean', help='remove build folders')
    c.add_argument('pack', nargs='?')
    c.set_defaults(fn=cmd_clean)
    return ap


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # a ROM dropped onto n64rx.exe
    if argv and pathlib.Path(argv[0]).suffix.lower() in romlib.EXTENSIONS and pathlib.Path(argv[0]).is_file():
        argv.insert(0, 'build')
    ap = parser()
    a = ap.parse_args(argv)
    if not getattr(a, 'fn', None):
        ap.print_help()
        return 0
    try:
        return a.fn(a)
    except N64rxError as e:
        print(f'\nn64rx: {e.message}', file=sys.stderr)
        if e.hint:
            print(f'What to do: {e.hint}', file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('\nn64rx: stopped.', file=sys.stderr)
        return 130
