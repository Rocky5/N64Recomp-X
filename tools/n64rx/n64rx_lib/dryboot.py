"""The dry boot (docs/roadmap.md B2): the ROM's boot run on this PC before
the first build, so a user's first build has what the console's learning
rounds used to find (code a Rare or Camelot game decompresses with the CPU,
code copied out of the boot image, code behind the TLB). tools/dryboot/
dryboot.c is a CPU-only N64 (the VR4300 and a register model of the RCP
that finishes every task at once); it runs the game's own entry code,
libultra and threads for a few seconds of game time and writes what the
console's safety net would have (learn.txt and learn_code.bin, learn.py's
formats) for the functions the analysis does not have. Nothing leaves the
build folder; the files are facts and the RDRAM pages the CPU wrote, as the
console's learn files are.

N64RX_DRYBOOT_SCRIPT=<autopad.txt>: the input from a script (docs/testing.md's
format) instead of the START/A schedule. N64RX_DRYBOOT=auto (the default: the
maintainer, 2026-10-23) runs it and uses what it learnt for a game that needs
it; 1 always uses it, 0 never runs it. N64RX_DRYBOOT_FRAMES: how long (1800
frames, 30 s of game time: a few seconds here). N64RX_DRYBOOT_DIR: prebuilt
dry boots (<dir>/<source hash>/dryboot), as the app's toolchain bundle ships
them (tools/xbox/pack_toolchain.py): its Windows MSYS2 has clang but no C
library to build one with."""
import hashlib
import os
import pathlib
import shutil
import subprocess

from . import engines

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / 'tools/dryboot/dryboot.c'
EXE = '.exe' if os.name == 'nt' else ''


def enabled():
    return os.environ.get('N64RX_DRYBOOT', 'auto') != '0'


# N64RX_DRYBOOT=auto: the dry boot runs, and what it learnt is used only when it found at least
# AUTO_MIN functions the analysis did not have: a game whose code the ROM does not hold as it runs
# (GoldenEye 1,515, Banjo-Kazooie 2,704, Mario Tennis 920, Donkey Kong 64 1,525, Castlevania LoD 133,
# Star Fox 64 98, Wave Race 64 48; without it the first three never start). Super Mario 64, Smash,
# Diddy Kong Racing, Mario Kart 64 and Ocarina learn 0, Kirby 64 1: their builds stay as they are.
AUTO_MIN = 16


def auto():
    return os.environ.get('N64RX_DRYBOOT', 'auto') == 'auto'


def worth(learn_file):
    """auto mode: whether the dry boot's learn file is worth using (AUTO_MIN functions)"""
    try:
        n = sum(1 for l in pathlib.Path(learn_file).read_text(encoding='utf-8', errors='replace').splitlines()
                if l.startswith('func '))
    except OSError:
        return False, 0
    return n >= AUTO_MIN, n


def tv_of_region(code):
    """osTvType from the header's region letter, as xbox/src/rt/boot.c reads it"""
    r = (code or 'E')[3:4]
    return 0 if r in 'DFHILPSUWXY' else 2 if r == 'B' else 1


def src_hash():
    """the dry boot's identity: its source's (a prebuilt one is used only for the same source)"""
    return hashlib.sha256(SRC.read_bytes()).hexdigest()[:12]


def _exe(log):
    """the dry boot built for this PC (once per source: build-host/dryboot/<hash>/), or the bundle's"""
    h = src_hash()
    pre = os.environ.get('N64RX_DRYBOOT_DIR')
    if pre and (pathlib.Path(pre) / h / f'dryboot{EXE}').is_file():
        return pathlib.Path(pre) / h / f'dryboot{EXE}'
    out = ROOT / 'build-host' / 'dryboot' / h / f'dryboot{EXE}'
    if out.exists():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        eng = engines.Native()
        rel_src, rel_out = SRC.relative_to(ROOT).as_posix(), out.relative_to(ROOT).as_posix()
        rc = eng.bash(f'clang -O2 -std=gnu11 -o {rel_out} {rel_src} -static', log, {})
    else:
        cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            return None
        with open(log, 'a', encoding='utf-8') as f:
            rc = subprocess.run([cc, '-O2', '-std=gnu11', '-o', str(out), str(SRC), '-lm'], stdout=f,
                                stderr=subprocess.STDOUT).returncode
    return out if rc == 0 and out.exists() else None


def run(rom, a, pack, bdir, prog, log):
    """the dry boot of rom (its image as the console runs it) with analysis a;
    the learn file it wrote, or None (nothing learnt, or no host compiler)"""
    exe = _exe(log)
    if exe is None:
        prog.note('dry boot skipped: no C compiler for this PC (N64RX_DRYBOOT)')
        return None
    d = pathlib.Path(bdir) / 'dryboot'
    d.mkdir(parents=True, exist_ok=True)
    rom_file = d / 'rom.z64'
    if not rom_file.exists() or rom_file.read_bytes() != rom.data:
        rom_file.write_bytes(rom.data)
    known = d / 'known.txt'
    known.write_text(''.join(f'{k & 0xFFFFFFFF:08x}\n' for k in sorted({k & 0xFFFFFFFF for k in a.all_funcs()})))
    boot = next((s for s in a.img.segments if s.name == 'boot'), None)
    frames = os.environ.get('N64RX_DRYBOOT_FRAMES', '1800')
    cmd = [str(exe), str(rom_file), str(d), '--entry', f'{a.entry:08x}',
           '--boot-size', str(pack.boot_size if pack else (boot.size if boot else 0x100000)),
           '--cic', str(rom.cic or 6102), '--tv', str(tv_of_region(rom.code)),
           '--rdram', str(pack.rdram_mb if pack and pack.rdram_mb in (4, 8) else 4),
           '--known', str(known), '--frames', frames,
           '--eep', '16' if pack and getattr(pack, 'save_type', '') == 'eep16k' else '4']
    if os.environ.get('N64RX_DRYBOOT_VERSIONS') in ('all', 'last'):   # which versions of a page (default: boot)
        cmd += ['--versions', os.environ['N64RX_DRYBOOT_VERSIONS']]
    if os.environ.get('N64RX_DRYBOOT_EXL', '0') != '0':   # calls made in exception context learnt too (as before)
        cmd += ['--exl-calls', '1']
    if os.environ.get('N64RX_DRYBOOT_TABLES', '1') == '0':   # a jr's jump table page not captured (as before)
        cmd += ['--jump-tables', '0']
    script = os.environ.get('N64RX_DRYBOOT_SCRIPT')   # an autopad script for its input, instead of the schedule
    if script:
        cmd += ['--script', script]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        prog.note('dry boot: stopped after 10 minutes (nothing learnt)')
        return None
    with open(log, 'a', encoding='utf-8') as f:
        f.write(' '.join(cmd) + '\n' + r.stderr)
    summary = next((l for l in r.stderr.splitlines() if l.startswith('dryboot: ') and 'frames' in l), '')
    if summary:
        prog.note(summary.replace('dryboot: ', 'dry boot: '))
    if r.returncode == 2:
        prog.note(next((l.replace('dryboot: ', 'dry boot: ') for l in r.stderr.splitlines() if 'does not know' in l), ''))
    learn = d / 'learn.txt'
    if r.returncode not in (0, 2) or not learn.exists():
        return None
    text = learn.read_text(encoding='utf-8', errors='replace')
    return learn if any(l.startswith(('func ', 'code ')) for l in text.splitlines()) else None
