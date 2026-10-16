"""n64rx build (PLAN.md 4.3): ROM -> pack -> N64Recomp -> compile -> XBE ->
the folder the user copies to the Xbox. Every step prints one progress line
with its time; the tools' own output goes to build-xbox/<id>/n64rx.log, and a
failure shows its tail and says what to do."""
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

from . import ROOT, VERSION, N64rxError, engines, packs, romonly, rom as romlib

GEN_VERSION = 3   # bump when what n64rx writes into gen/ changes (the cache key)
VIDEO = {'auto': 0, '480': 2, '720': 3}
EEPROM = {'eep4k': 512, 'eep16k': 2048}
FATX_BAD = '<>:"/\\|?*'


def cache_dir():
    base = os.environ.get('LOCALAPPDATA') if engines.IS_WINDOWS else None
    d = pathlib.Path(base) / 'n64rx' if base else pathlib.Path.home() / '.n64rx'
    d.mkdir(parents=True, exist_ok=True)
    return d


def folder_name(title):
    """a FATX-safe folder name: 42 characters, none of <>:"/\\|?*"""
    name = ''.join('_' if c in FATX_BAD or ord(c) < 32 else c for c in title).strip(' .')
    return (name or 'game')[:42]


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


class Progress:
    def __init__(self, out, total):
        self.out, self.total, self.n, self.t = out, total, 0, None

    def step(self, text):
        self.done()
        self.n += 1
        self.t = time.monotonic()
        self.out(f'[{self.n}/{self.total}] {text}')

    def note(self, text):
        self.out(f'      {text}')

    def done(self):
        if self.t is not None:
            self.out(f'      done in {time.monotonic() - self.t:.1f} s')
            self.t = None


def tail(log, n=25):
    try:
        lines = pathlib.Path(log).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return ''
    return '\n'.join('      | ' + l for l in lines[-n:])


def cache_key(rom, pack, gpr=None, pin=None):
    """the recompile step is skipped when this is unchanged: the ROM, every
    file of the pack, the recompiler's pin, what n64rx writes, the width"""
    return hashlib.sha256(json.dumps([rom.sha1, pack.file_hash(), pin or engines.recompiler_pin(), GEN_VERSION,
                                      gpr or '']).encode()).hexdigest()


def decide_gpr(pack, rom, override, prog):
    if override:
        return int(override)
    if pack.gpr in ('32', '64'):
        return int(pack.gpr)
    sys.path.insert(0, str(ROOT / 'tools/xbox'))
    try:
        import tomllib
        import recomp_scan64
        syms = tomllib.loads(_syms_path(pack).read_text(encoding='utf-8'))
        ignored = set(tomllib.loads((pack.dir / 'recomp.toml').read_text(encoding='utf-8'))
                      .get('patches', {}).get('ignored', []))
        for sec in syms.get('section', []):   # what is not recompiled does not count
            sec['functions'] = [f for f in sec.get('functions', []) if f['name'] not in ignored]
        r = recomp_scan64.scan(rom.data, syms)
    except ImportError:
        prog.note('rabbitizer is not installed (pip install rabbitizer): 64-bit registers, the safe default')
        return 64
    finally:
        sys.path.pop(0)
    if r['kseg1']:
        prog.note(f"{len(r['kseg1'])} KSEG1 sites (see the log); the pack patches them")
    bits = 32 if not r['hits'] else 64
    prog.note(f"registers: {bits}-bit ({len(r['hits'])} 64-bit instructions in {r['functions']} functions)")
    return bits


def _syms_path(pack):
    import tomllib
    cfg = tomllib.loads((pack.dir / 'recomp.toml').read_text(encoding='utf-8'))
    return pack.dir / cfg['input']['symbols_file_path']


REFUSED = re.compile(r'Error recompiling (\S+)')


def _refused(log, start):
    with open(log, 'rb') as f:
        f.seek(start)
        return sorted(set(REFUSED.findall(f.read().decode('utf-8', 'replace'))))


def _ignore_dead(pack, ainfo, refused, prog):
    """functions the recompiler refused (PLAN.md 3.14): left out when only
    replaced libultra reaches them, handed to the runtime's interpreter
    when the game runs them"""
    by_name = {n: x for x, n in ainfo['names'].items()}
    live = [n for n in refused if by_name.get(n) in ainfo['live']]
    if any(n not in by_name for n in refused):
        return False
    if live:
        with open(pack.dir / 'interp.txt', 'a', encoding='utf-8') as f:
            f.write(''.join(f'{n} 0x{by_name[n]:08X}\n' for n in live))
        prog.note(f'interpreted (the recompiler refused them): {", ".join(live)}')
    cfgp = pack.dir / 'recomp.toml'
    text = cfgp.read_text(encoding='utf-8')
    m = re.search(r'^ignored = \[(.*)\]$', text, re.M)
    have = set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()
    have |= set(refused)
    text = re.sub(r'^ignored = \[.*\]$', lambda _m: 'ignored = [' + ', '.join(f'"{n}"' for n in sorted(have)) + ']',
                  text, flags=re.M)
    cfgp.write_text(text, encoding='utf-8', newline='\n')
    dead = [n for n in refused if n not in live]
    if dead:
        prog.note(f'left out (dead, refused by the recompiler): {", ".join(dead)}')
    return True


def write_interp_c(pack, gen):
    """the bodies of the routines the runtime interprets (pack/interp.txt,
    xbox/src/rt/interp.c): each enters the interpreter at its address, and
    the table puts them where function pointers look"""
    p = pack.dir / 'interp.txt'
    rows = []
    if p.exists():
        for line in p.read_text(encoding='utf-8').split('\n'):
            if line.strip():
                n, v = line.split()
                rows.append((n, int(v, 16)))
    rows = sorted(set(rows), key=lambda r: r[1])
    out = ['/* generated by n64rx: hand-written routines the runtime interprets (PLAN.md 3.14) */',
           '#include "recomp.h"', 'void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);', '']
    for n, v in rows:
        out.append(f'void {n}(uint8_t* rdram, recomp_context* ctx) {{ xrc_interp(rdram, ctx, 0x{v:08X}u); }}')
    if rows:
        out += ['', 'typedef struct { uint32_t vram; recomp_func_t* fn; } XrcInterpEntry;',
                'const XrcInterpEntry xrc_interp_table[] = {']
        out += [f'    {{ 0x{v:08X}u, {n} }},' for n, v in rows]
        out += ['};', f'const unsigned xrc_interp_count = {len(rows)};']
    (gen / 'xrc_interp.c').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    return len(rows)


def write_recomp_config(pack, rom_file, gen, cfg_dir):
    text = (pack.dir / 'recomp.toml').read_text(encoding='utf-8')
    text = text.replace('@ROM@', rom_file.resolve().as_posix()).replace('@OUT@', gen.resolve().as_posix())
    text = re.sub(r'^(\s*symbols_file_path\s*=\s*)"([^"]+)"', lambda m: f'{m.group(1)}"{(pack.dir / m.group(2)).resolve().as_posix()}"',
                  text, flags=re.M)
    cfg_dir.mkdir(parents=True, exist_ok=True)
    cfg = cfg_dir / 'recomp.toml'
    cfg.write_text(text, encoding='utf-8', newline='\n')
    return cfg


CALL = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\(rdram, ctx\);')
DECL = re.compile(r'^void ([A-Za-z_][A-Za-z0-9_]*)\(uint8_t\* rdram, recomp_context\* ctx\);', re.M)


def write_extern_h(gen):
    """prototypes for what the generated code calls and funcs.h does not
    declare: the libultra entry points the runtime provides (<name>_recomp),
    the ignored functions a pack provides. Upstream compiles with implicit
    declarations allowed; here they are errors (a float result would be read
    from EAX), and every one of these calls has the same signature."""
    declared = set(DECL.findall((gen / 'funcs.h').read_text(encoding='utf-8')))
    called = set()
    for c in gen.glob('funcs_*.c'):
        called.update(CALL.findall(c.read_text(encoding='utf-8', errors='replace')))
    extern = sorted(called - declared - {'LOOKUP_FUNC'})
    body = ''.join(f'void {n}(uint8_t* rdram, recomp_context* ctx);\n' for n in extern)
    (gen / 'xrc_extern.h').write_text('/* generated by n64rx: what the generated code calls that funcs.h does not\n'
                                      " * declare (the runtime's libultra, a pack's host functions) */\n"
                                      '#pragma once\n#include "recomp.h"\n' + body, encoding='utf-8', newline='\n')
    return extern


def write_game_c(pack, gpr_bits, gen):
    from PIL import Image
    img = Image.open(pack.icon).convert('RGBA').resize((64, 64), Image.LANCZOS)
    px = img.tobytes()
    words = [(px[i + 3] << 24) | (px[i] << 16) | (px[i + 1] << 8) | px[i + 2] for i in range(0, len(px), 4)]
    rows = ',\n'.join('    ' + ', '.join(f'0x{w:08X}u' for w in words[i:i + 8]) for i in range(0, len(words), 8))
    (gen / 'xrc_icon.c').write_text(
        f'/* generated by n64rx from {pack.icon.name}: the pack\'s own art, 64x64 ARGB */\n#include <stdint.h>\n'
        f'const uint32_t xrc_icon_64[4096] = {{\n{rows}\n}};\n', encoding='utf-8', newline='\n')
    (gen / 'xrc_game.c').write_text(f'''/* generated by n64rx {VERSION} from games/{pack.id}/game.toml: facts about the
 * game, no byte of the ROM (PLAN.md 4.2) */
#include "xrc_game.h"
#include "funcs.h"
#include "recomp_overlays.inl"

extern const uint32_t xrc_icon_64[4096];

const XrcGame xrc_game = {{
    .id = {c_str(pack.id)},
    .title = {c_str(pack.title)},
    .title_id = 0x{pack.title_id:08X}u,
    .rdram_mb = {pack.rdram_mb},
    .entrypoint = 0x{pack.entrypoint:08X}u,
    .boot_size = 0x{pack.boot_size:X}u,
    .gpr_bits = {gpr_bits},
    .test_rom = {1 if pack.test else 0},
    .entry = recomp_entrypoint,
    .sections = section_table,
    .num_sections = ARRLEN(section_table),
    .icon = xrc_icon_64,
    .code = {c_str(pack.code)},
    .rom_size = {pack.rom_size}u,
    .sha1 = {{ {''.join(c_str(h) + ', ' for h in pack.sha1[:3])}0 }},
    .eeprom_bytes = {EEPROM.get(pack.save_type, 0)}u,
}};
''', encoding='utf-8', newline='\n')


def readme(pack, folder, rom_beside=False):
    where = ('The ROM next to it is yours,\nnormalised; the game reads it from here and checks its SHA-1.'
             if rom_beside else
             'Your ROM is inside default.xbe\n(normalised); the game reads it from there and checks its SHA-1.')
    return f'''{pack.title} for the original Xbox, built by N64Recomp-X {VERSION}

Copy this whole folder to your Xbox, for example to
    E:\\Games\\{folder}\\    or    F:\\Games\\{folder}\\
and start default.xbe from your dashboard. {where}

On the console:
  BACK                     the settings menu (video, widescreen, volume, ...)
  hold BACK while loading  safe video: 480i 4:3, saved
  hold BACK + START        safe video and default settings
  L + R + BACK + BLACK     back to the dashboard, anywhere
  L + R + BACK + WHITE     restart, anywhere

Saves, settings and logs: E:\\UDATA\\{pack.title_id:08X}\\
This folder contains your game's code: keep it to yourself.
'''


def build(rom_path, out=None, engine="auto", video="auto", fast=False, gpr=None, echo=None, eng=None,
          analyze=False, rom_beside=False):
    echo = echo or (lambda s: print(s, flush=True))
    prog = Progress(echo, 5)
    prog.step('Reading the ROM')
    rom = romlib.read(rom_path)
    try:
        pack = packs.match(rom, packs.all_packs())
    except N64rxError:
        pack = None   # no pack: the ROM alone (PLAN.md 3.14)
    eng = eng or engines.pick(engine)
    out_root = pathlib.Path(out) if out else pathlib.Path.cwd() / 'out'
    if eng.name == 'docker':
        return _build_in_docker(eng, rom, out_root, video, fast, gpr, prog, rom_beside)
    ainfo = None
    if analyze or pack is None:
        base = pack
        bdir = ROOT / 'build-xbox' / ((base.id + '-rom') if base else re.sub(r'[^a-z0-9]', '', rom.code.lower()))
        bdir.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        pack, ainfo = romonly.make(rom, bdir, base)
        a = ainfo['analysis']
        prog.note(f'{pack.title}: {rom.describe()}, {rom.order} byte order, '
                  + ('analysed from the ROM over its pack' if base else 'no pack: analysed from the ROM alone'))
        prog.note(f'{len(a.all_funcs())} functions in {len(a.img.segments)} segments, {len(ainfo["ids"])} libultra '
                  f'functions named, in {time.time() - t0:.1f} s ({bdir / "analysis.txt"})')
    else:
        prog.note(f'{pack.title}: {rom.describe()}, {rom.order} byte order, SHA-1 known to the pack')
        bdir = ROOT / 'build-xbox' / pack.id
    bdir.mkdir(parents=True, exist_ok=True)
    log = bdir / 'n64rx.log'
    log.write_text(f'n64rx {VERSION} build {rom.path} ({engines.host_summary()}, engine {eng.name})\n', encoding='utf-8')
    rom_file = bdir / 'rom.z64'
    if not rom_file.exists() or rom_file.read_bytes() != rom.data:
        rom_file.write_bytes(rom.data)

    prog.step('Recompiling the game\'s code (N64Recomp)')
    eng.ensure_recompiler(log)
    gen = bdir / 'gen'
    key = cache_key(rom, pack, gpr)
    stamp = gen / '.n64rx-key'
    if stamp.exists() and stamp.read_text() == key and (gen / 'funcs.h').exists():
        prog.note('the same ROM, pack and recompiler as last time: kept')
    else:
        if gen.exists():
            shutil.rmtree(gen)
        for _attempt in range(40):
            cfg = write_recomp_config(pack, rom_file, gen, bdir / 'cfg')
            start = log.stat().st_size
            if not eng.recompile(cfg, log) and (gen / 'funcs.h').exists():
                break
            refused = _refused(log, start)
            if not ainfo or not refused or not _ignore_dead(pack, ainfo, refused, prog):
                raise N64rxError('N64Recomp failed on this ROM.', f'the log says why:\n{tail(log)}\nfull log: {log}')
            if gen.exists():
                shutil.rmtree(gen)
        extern = write_extern_h(gen)
        ninterp = write_interp_c(pack, gen)
        if ninterp:
            prog.note(f"{ninterp} hand-written routines run interpreted")
        prog.note(f'{len(extern)} functions the runtime or the pack provides')
        bits = decide_gpr(pack, rom, gpr, prog)
        write_game_c(pack, bits, gen)
        (gen / '.n64rx-bits').write_text(str(bits))
        stamp.write_text(key)
    bits = int((gen / '.n64rx-bits').read_text())
    nfunc = (gen / 'funcs.h').read_text().count('(uint8_t* rdram, recomp_context* ctx);')
    nfiles = len(list(gen.glob('funcs_*.c')))
    prog.note(f'{nfunc} functions in {nfiles} files, {bits}-bit registers')

    prog.step(f'Compiling for the Xbox ({nfiles} generated files + the runtime{", fast" if fast else ", ThinLTO"})')
    flags = ['-DXRC_GPR32=1'] if bits == 32 else []
    env = {
        'XBOX_BUILD_DIR': engines.msys_path(bdir / 'xbox') if engines.IS_WINDOWS else str(bdir / 'xbox'),
        'XRC_GEN_DIR': engines.msys_path(gen) if engines.IS_WINDOWS else str(gen),
        'XRC_TITLE_ID': pack.title_id_hex,
        'XRC_XBE_TITLE': pack.title,
        'XRC_GAME_CFLAGS': ' '.join(flags),
        # N64RX_CFLAGS: a developer's extra flags (a test build: -DXHW_AUTOPAD=1), docs/testing.md
        'XBOX_CFLAGS': f'-DXRC_VIDEO_DEFAULT={VIDEO[video]}' + (' -O1' if fast else '')
                       + (' ' + os.environ['N64RX_CFLAGS'] if os.environ.get('N64RX_CFLAGS') else ''),
        'XBOX_LTO': '0' if fast else '1',
    }
    if eng.build_xbe(env, log) or not (bdir / 'xbox/xbe/default.xbe').exists():
        raise N64rxError('Compiling or linking the XBE failed.', f'the log says why:\n{tail(log)}\nfull log: {log}')

    prog.step('Finishing the XBE (title ID, icon' + (')' if rom_beside else ', the ROM inside)'))
    xbe = bdir / 'xbox/xbe/default.xbe'
    folder = out_root / folder_name(pack.title)
    if folder.exists():
        shutil.rmtree(folder)
    (folder / 'debug').mkdir(parents=True)
    shutil.copy2(xbe, folder / 'default.xbe')
    r = subprocess.run([sys.executable, str(ROOT / 'tools/xbox/xbe_sections.py'), str(folder / 'default.xbe'),
                        '--title-id', pack.title_id_hex, '--name', pack.title, '--icon', str(pack.icon),
                        '--tbn', str(folder / 'default.tbn')]
                       # the ROM as a $ROM section the kernel never loads (PLAN.md 3.5); --rom-beside: a file
                       + ([] if rom_beside else ['--rom', str(rom_file)]), capture_output=True, text=True)
    with open(log, 'a', encoding='utf-8') as f:
        f.write(r.stdout + r.stderr)
    if r.returncode:
        raise N64rxError('The XBE pass failed.', f'{r.stderr.strip()}\nPillow is needed: pip install pillow')

    prog.step('Writing the folder for the Xbox')
    if rom_beside:
        (folder / f'{folder_name(pack.title)}.z64').write_bytes(rom.data)
    (folder / 'README.txt').write_text(readme(pack, folder.name, rom_beside), encoding='utf-8', newline='\r\n')
    shutil.copy2(bdir / 'xbox/n64recomp_x.map', folder / 'debug/n64recomp_x.map')
    prog.done()
    (cache_dir() / 'last.json').write_text(json.dumps({'pack': pack.id, 'log': str(log), 'folder': str(folder),
                                                        'time': time.time()}))
    echo(f'\nDone: {folder}')
    echo(f'Copy that folder to your Xbox (E:\\Games\\{folder.name}\\) and start default.xbe.')
    return folder


def _build_in_docker(eng, rom, out_root, video, fast, gpr, prog, rom_beside=False):
    out_root.mkdir(parents=True, exist_ok=True)
    log = cache_dir() / 'docker.log'
    log.write_text('', encoding='utf-8')
    args = ['build', f'/rom/{rom.path.name}', '--out', '/out', '--engine', 'native', '--video', video]
    if fast:
        args.append('--fast')
    if gpr:
        args += ['--gpr', str(gpr)]
    if rom_beside:
        args.append('--rom-beside')
    prog.note(f'in the SDK image {engines.image_name()} (the first build pulls it)')
    if eng.run_inside(args, rom.path.resolve().parent, out_root.resolve(), log):
        raise N64rxError('The build inside Docker failed.', f'{tail(log)}\nfull log: {log}')
    prog.done()
    return out_root
