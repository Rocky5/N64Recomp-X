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
import struct
import subprocess
import sys
import time

from . import ROOT, VERSION, N64rxError, compat, dryboot, engines, packs, romonly, rom as romlib, saves
from . import learn as learnmod

GEN_VERSION = 13   # bump when what n64rx writes into gen/ changes (the cache key)
VIDEO = {'auto': 0, '480': 2, '720': 3}
EEPROM = {'eep4k': 512, 'eep16k': 2048}
FATX_BAD = '<>:"/\\|?*'


def cache_dir():
    """n64rx's own folder: %LOCALAPPDATA%\\n64rx or ~/.n64rx, or N64RX_HOME (the portable app's data
    folder next to it, docs/app.md)"""
    base = os.environ.get('LOCALAPPDATA') if engines.IS_WINDOWS else None
    home = os.environ.get('N64RX_HOME')
    d = pathlib.Path(home) if home else pathlib.Path(base) / 'n64rx' if base else pathlib.Path.home() / '.n64rx'
    d.mkdir(parents=True, exist_ok=True)
    return d


WIN_DEVICES = {'CON', 'PRN', 'AUX', 'NUL'} | {f'COM{i}' for i in range(1, 10)} | {f'LPT{i}' for i in range(1, 10)}


def folder_name(title):
    """a FATX-safe folder name: 42 characters, none of <>:"/\\|?*, and not a
    Windows device name (CON, NUL, COM1: the folder's mkdir failed after the
    whole build; docs/review-2026-10.md 3.r9)"""
    name = ''.join('_' if c in FATX_BAD or ord(c) < 32 else c for c in title).strip(' .')
    name = (name or 'game')[:42]
    if name.split('.')[0].upper() in WIN_DEVICES:
        name = ('_' + name)[:42]
    return name


OUTPUT_MARK = '.n64rx-output'


def output_folder(out_root, name):
    """the folder to write the build into, made here: an existing folder of
    that name is replaced only when n64rx made it (it holds OUTPUT_MARK);
    another one is left alone and the build goes to "<name> (2)" and on. The
    name comes from the ROM's header, which anyone can edit: a header saying
    "My Games" deleted Documents\\My Games (docs/review-2026-10.md 3.1)"""
    for k in range(1, 100):
        folder = out_root / (name if k == 1 else f'{name[:36]} ({k})')
        # (a folder an earlier n64rx made has no mark: its XBE and its map say so)
        ours = folder.is_dir() and ((folder / OUTPUT_MARK).is_file() or
                                    ((folder / 'default.xbe').is_file() and (folder / 'debug/n64recomp_x.map').is_file()))
        if folder.exists() and not ours:
            continue
        if folder.exists():
            shutil.rmtree(folder)
        (folder / 'debug').mkdir(parents=True)
        (folder / OUTPUT_MARK).write_text('made by n64rx: a build folder it may replace\n', encoding='utf-8')
        return folder
    raise N64rxError(f'Every folder name for this build in {out_root} is taken by folders n64rx did not make.',
                     'choose another output folder (--out).')


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


class Progress:
    """a line per step, note and step time; with events (the app's
    --events json, cli.py) the same as dicts instead"""

    def __init__(self, out, total, events=None):
        self.out, self.total, self.n, self.t, self.events = out, total, 0, None, events

    def step(self, text):
        self.done()
        self.n += 1
        self.t = time.monotonic()
        if self.events:
            self.events({'ev': 'step', 'n': self.n, 'of': self.total, 'text': text})
        else:
            self.out(f'[{self.n}/{self.total}] {text}')

    def note(self, text):
        if self.events:
            self.events({'ev': 'note', 'text': text})
        else:
            self.out(f'      {text}')

    def done(self):
        if self.t is not None:
            secs = time.monotonic() - self.t
            if self.events:
                self.events({'ev': 'done', 'secs': round(secs, 1)})
            else:
                self.out(f'      done in {secs:.1f} s')
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
                                      gpr or '', os.environ.get('N64RX_TRACE', ''), pack.rdram_mb, os.environ.get('N64RX_GUARD', '1'),
                                      os.environ.get('N64RX_RESTRICT', '0'), os.environ.get('N64RX_FR', '1')]).encode()).hexdigest()


def decide_gpr(pack, rom, override, prog, data=None):
    """`data`: the bytes N64Recomp reads (the ROM plus code learnt from RDRAM past its end)"""
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
        r = recomp_scan64.scan(data if data is not None else rom.data, syms)
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


def BUILD_ROOT():
    """where a game's build folder goes: build-xbox/, or N64RX_BUILD_ROOT (the library sweep's own,
    so it does not build over a game being worked on)"""
    root = os.environ.get('N64RX_BUILD_ROOT')   # relative: to the working directory, made absolute (N64Recomp runs elsewhere)
    return pathlib.Path(root).resolve() if root else ROOT / 'build-xbox'


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
    if 'recomp_entrypoint' in refused:   # the function the recompiler was given as its entrypoint
        a = ainfo['analysis']
        cfgp = pack.dir / 'recomp.toml'
        text = cfgp.read_text(encoding='utf-8')
        cur = int(re.search(r'^entrypoint = (0x[0-9A-Fa-f]+)', text, re.M).group(1), 16)
        refused = [ainfo['names'][cur] if n == 'recomp_entrypoint' else n for n in refused]
        if cur == a.entry:   # the entry itself (Turok 2's sets up the TLB): interpreted, started by its stub
            with open(pack.dir / 'interp.txt', 'a', encoding='utf-8') as f:
                f.write(f'xrc_entry_interp 0x{a.entry:08X}\n')
            (pack.dir / 'entry_interp').write_text(f'0x{a.entry:08X}\n', encoding='utf-8')
            prog.note('the entry code is interpreted (the recompiler refused it)')
            live_entry = ainfo['names'][a.entry]
        else:
            live_entry = None
        # the recompiler is given no entrypoint then (its own must sit at ROM 0x1000, where only the
        # refused entry does); the runtime starts the game through the interpreter's stub
        text = re.sub(r'^entrypoint = .*\n', '', text, flags=re.M)
        cfgp.write_text(text, encoding='utf-8', newline='\n')
    else:
        live_entry = None
    # a static N64Recomp made for a jal target no function holds (static_<section>_<vram>) and then refused:
    # the live functions that jal there are interpreted instead (Pokemon Puzzle League)
    statics = [int(m.group(1), 16) for n in refused for m in [re.fullmatch(r'static_\d+_([0-9A-F]{8})', n)] if m]
    if statics:
        from .analyze.mips import Ins, words
        a = ainfo['analysis']
        callers = set()
        ovl = a.overlays()
        targets = set(statics)
        # a static can be reached from another static (made for a tail j: Mario Tennis's captured
        # versions): the code outside every function that jal's or j's a target is a static too, whose
        # start (the nearest call target at or before it) is followed back to a real caller
        for _round in range(6):
            new_t = set()
            for seg in a.img.segments:
                fs = a.funcs.get(seg.name, {})
                spans = sorted((f.start, f.end) for f in fs.values())
                for x0, f in fs.items():
                    x = a.key(seg, x0, ovl)   # an overlay's (a captured version's) functions by their full key
                    if x not in ainfo['live'] or x not in ainfo['names']:
                        continue
                    ws = words(a.img.rom, seg.rom + (f.start - seg.vram), (f.end - f.start) // 4)
                    if any((Ins(w, f.start + 4 * i).is_jal or Ins(w, f.start + 4 * i).is_j) and
                           Ins(w, f.start + 4 * i).target in targets for i, w in enumerate(ws)):
                        callers.add(ainfo['names'][x])
                if not any(seg.has(t) for t in targets):
                    continue
                ws = words(a.img.rom, seg.rom, seg.size // 4)
                calls = sorted({Ins(w, seg.vram + 4 * i).target for i, w in enumerate(ws)
                                if Ins(w, seg.vram + 4 * i).is_jal or Ins(w, seg.vram + 4 * i).is_j} & set(range(seg.vram, seg.vram + seg.size, 4)))
                for i, w in enumerate(ws):
                    pc = seg.vram + 4 * i
                    ins = Ins(w, pc)
                    if not (ins.is_jal or ins.is_j) or ins.target not in targets or                             any(lo <= pc < hi for lo, hi in spans):
                        continue
                    starts = [c for c in calls if c <= pc and c not in fs]
                    if starts and starts[-1] not in targets:
                        new_t.add(starts[-1])
            if not new_t:
                break
            targets |= new_t
        refused = [n for n in refused if not re.fullmatch(r'static_\d+_[0-9A-F]{8}', n)] + sorted(callers)
        if not callers:
            return False
    live = [n for n in refused if by_name.get(n) in ainfo['live'] and n != live_entry]
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


GUARD_DEF = re.compile(r'^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{$', re.M)


def _fnv(data):
    """FNV-1a over big-endian words (guard.c's page_hash)"""
    h = 0x811C9DC5
    for i in range(0, len(data), 4):
        h = ((h ^ int.from_bytes(data[i:i + 4], 'big')) * 0x01000193) & 0xFFFFFFFF
    return h


def write_guard(pack, gen, data):
    """code captured from RDRAM (the code_* segments) runs recompiled only
    while RDRAM still holds it (xbox/src/rt/guard.c): each function's hash
    of the bytes it was recompiled from goes to gen/xrc_guard.c (a page's
    would change with the data beside the code: Banjo-Kazooie's core code
    shares its first page with data), and each function is wrapped to check
    its state first. Several versions of a function at one vram (learn.py's
    code_<vram>_v<n> overlays: the scenes Mario Tennis loads there) are
    entries side by side with their bodies, so the guard runs the one whose
    bytes RDRAM holds. N64RX_GUARD=0: no wrappers. Returns the number wrapped."""
    import tomllib
    entries = {}   # name -> (vram, size, hash)
    learnt = set()   # entries of the learnt DMA overlays (learn_*): checked again every retrace
    # learn.py's learn_* segments (an overlay the console saw a DMA load) are guarded too: a game that
    # decompresses its next scene's code over one with the CPU (Star Fox 64's title over its intro, at
    # 0x80187520) sends no DMA the runtime's overlay switch sees, and the intro's recompiled code ran
    # there. N64RX_GUARD_LEARN=0: code_* only, as before
    prefixes = ('.code_', '.learn_') if os.environ.get('N64RX_GUARD_LEARN', '1') != '0' else ('.code_',)
    if os.environ.get('N64RX_GUARD', '1') != '0':
        syms = tomllib.loads(_syms_path(pack).read_text(encoding='utf-8'))
        for sec in syms.get('section', []):
            if not sec['name'].startswith(prefixes):
                continue
            v0, r0 = sec['vram'], sec['rom']
            for f in sec.get('functions', []):
                v, n = f['vram'], max(f.get('size', 4), 4)
                if f['name'] not in entries:
                    entries[f['name']] = (v, n, _fnv(data[r0 + v - v0:r0 + v - v0 + n]))
                    if sec['name'].startswith('.learn_'):
                        learnt.add(f['name'])
    names = sorted(entries, key=lambda nm: (entries[nm][0], nm))
    order = [entries[nm][0] for nm in names]
    index = {nm: (i, entries[nm][0]) for i, nm in enumerate(names)}
    wrapped, bodies = 0, set()
    for f in sorted(gen.glob('funcs_*.c')):
        text = f.read_text(encoding='utf-8')
        hit = [m for m in GUARD_DEF.finditer(text) if m.group(1) in index]
        if not hit:
            continue
        out, last = [], 0
        for k, m in enumerate(hit):
            name = m.group(1)
            i, v = index[name]
            out.append(text[last:m.start()])
            if k == 0:
                out.append('extern volatile unsigned char xrc_guard_state[];\n'
                           'void xrc_guard_miss(uint8_t* rdram, recomp_context* ctx, uint32_t vram, unsigned i, '
                           'recomp_func_t* body);\n')
            out.append(f'void {name}__g(uint8_t* rdram, recomp_context* ctx);\n'
                       f'RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx) {{\n'
                       f'    if (xrc_guard_state[{i}] == 1) {name}__g(rdram, ctx);\n'
                       f'    else xrc_guard_miss(rdram, ctx, 0x{v:08X}u, {i}u, {name}__g);\n'
                       f'}}\n'
                       f'void {name}__g(uint8_t* rdram, recomp_context* ctx) {{')
            last = m.end()
            wrapped += 1
            bodies.add(name)
        out.append(text[last:])
        f.write_text(''.join(out), encoding='utf-8', newline='\n')
    n = max(len(order), 1)
    rows = lambda vals: ',\n'.join('    ' + ', '.join(f'0x{x:08X}u' for x in vals[i:i + 8]) for i in range(0, len(vals), 8)) or '    0'
    # the bodies only where one vram has several versions: the guard's other choices there
    import collections
    many = {v for v, c in collections.Counter(order).items() if c > 1}
    multi = [nm for nm in names if entries[nm][0] in many and nm in bodies]
    decl = ''.join(f'void {nm}__g(uint8_t* rdram, recomp_context* ctx);\n' for nm in multi)
    body = ',\n'.join(f'    {nm}__g' if nm in multi else '    0' for nm in names) or '    0'
    (gen / 'xrc_guard.c').write_text(
        '/* generated by n64rx: the captured functions\' hashes as recompiled (xbox/src/rt/guard.c) */\n'
        '#include "recomp.h"\n'
        f'const uint32_t xrc_guard_vram[{n}] = {{\n{rows(order)}\n}};\n'
        f'const uint32_t xrc_guard_size[{n}] = {{\n{rows([entries[nm][1] for nm in names])}\n}};\n'
        f'const uint32_t xrc_guard_hash[{n}] = {{\n{rows([entries[nm][2] for nm in names])}\n}};\n'
        f'const unsigned xrc_guard_count = {len(order)}u;\n'
        f'volatile unsigned char xrc_guard_state[{n}];\n'
        f'{decl}'
        f'recomp_func_t* const xrc_guard_body[{n}] = {{\n{body}\n}};\n'
        f'const unsigned char xrc_guard_retrace_each[{n}] = {{\n    '
        + (', '.join('1' if nm in learnt else '0' for nm in names) or '0') + '\n};\n',
        encoding='utf-8', newline='\n')
    return wrapped


SP_CALL = re.compile(r'^(\s*)((?:func_|static_)\w+|\w+_recomp|LOOKUP_FUNC\([^)]*\))\(rdram, ctx\);$')
SP_SITE = re.compile(r'^\s*// 0x([0-9A-F]{8}):')

RESTRICT_DEF = re.compile(r'^((?:RECOMP_FUNC )?void \w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{$')
RESTRICT_LABEL = re.compile(r'^\s*(L_[0-9A-Fa-f]+):')
RESTRICT_GOTO = re.compile(r'\bgoto (L_[0-9A-Fa-f]+);')


def write_restrict(gen):
    """`restrict` on both parameters of every generated function (docs/roadmap.md
    B6). The generated code keeps the MIPS registers in ctx->rN and, built with
    -fno-strict-aliasing, LLVM must assume any RDRAM store may change them, so
    it reloads them after every store: a four-instruction fill loop became 6 to
    15 x86 instructions with 4 to 8 memory operands, 4 register-only ones with
    restrict. Safe because every context access is an offset from ctx
    (recomp.h's XRC_FODD_ARRAY) and nothing reaches RDRAM through ctx. Each
    backward goto (a loop) gets a compiler barrier first: a game's spin on a
    word another thread sets (Kirby 64's wait for its music) reloaded the word
    only because the register store beside it might have aliased it, and with
    restrict LLVM could hoist the load and spin for ever. Definitions only:
    a parameter's restrict is not part of the function's type, so funcs.h
    stays as it is. N64RX_RESTRICT=1 to use it (0, the default until the M3
    gate measures it on a console). Returns the number of functions."""
    if os.environ.get('N64RX_RESTRICT', '0') == '0':
        return 0
    n = 0
    for f in sorted(gen.glob('funcs_*.c')):
        lines = f.read_text(encoding='utf-8', errors='replace').split('\n')
        seen = set()
        for i, line in enumerate(lines):
            m = RESTRICT_DEF.match(line)
            if m:
                lines[i] = m.group(1) + '(uint8_t* restrict rdram, recomp_context* restrict ctx) {'
                seen = set()
                n += 1
                continue
            lm = RESTRICT_LABEL.match(line)
            if lm:
                seen.add(lm.group(1))
            if 'goto L_' in line:
                lines[i] = RESTRICT_GOTO.sub(
                    lambda g: f'{{ XRC_LOOP_BARRIER(); goto {g.group(1)}; }}' if g.group(1) in seen else g.group(0), line)
        f.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    return n


def write_fodd(gen, fr):
    """In a game whose threads run with Status.FR (saves.thread_fr), the
    generator's ctx->f_odd[(i - 1) * 2] (an odd FPR's word as FR = 0 has it:
    the even register's high half) becomes XRC_FODD(ctx)[(i - 1) * 2], which
    follows the context's FR (recomp.h). Returns the number of accesses."""
    if not fr:
        return 0
    n = 0
    for f in sorted(gen.glob('funcs_*.c')):
        text = f.read_text(encoding='utf-8', errors='replace')
        k = text.count('ctx->f_odd[(')
        if k:
            f.write_text(text.replace('ctx->f_odd[(', 'XRC_FODD(ctx)[('), encoding='utf-8', newline='\n')
            n += k
    return n


def write_sp_check(gen):
    """diagnosis (N64RX_SP_CHECK=1): every call in the generated code checks that $sp comes back as it
    went (trace.c xrc_sp_bad logs the call site and the callee when it does not: a callee that left
    its frame behind; Castlevania: Legacy of Darkness's crash). Returns the number of calls wrapped"""
    n = 0
    for f in sorted(gen.glob('funcs_*.c')):
        lines = f.read_text(encoding='utf-8').split('\n')
        site, out = 0, ['void xrc_sp_bad(uint32_t site, const char* callee, uint32_t target, uint32_t before, uint32_t after);']
        for ln in lines:
            m = SP_SITE.match(ln)
            if m:
                site = int(m.group(1), 16)
            m = SP_CALL.match(ln)
            if m:
                ind, callee = m.group(1), m.group(2)
                tg = callee[len('LOOKUP_FUNC('):-1] if callee.startswith('LOOKUP_FUNC(') else '0'
                ln = (f'{ind}{{ gpr xrc_sp0 = ctx->r29; uint32_t xrc_tg = (uint32_t)({tg}); {callee}(rdram, ctx); '
                      f'if (ctx->r29 != xrc_sp0) xrc_sp_bad(0x{site:08X}u, "{callee.replace(chr(34), "")}", xrc_tg, '
                      f'(uint32_t)xrc_sp0, (uint32_t)ctx->r29); }}')
                n += 1
            out.append(ln)
        f.write_text('\n'.join(out), encoding='utf-8', newline='\n')
    return n


SETJMP_CALL = re.compile(r'^(\s*)n64_setjmp\(rdram, ctx\);$', re.M)


def write_setjmp_sites(gen):
    """each call to the game's setjmp (romonly: n64_setjmp) becomes XRC_SETJMP, a host setjmp in the
    calling function (recomp.h, xbox/src/rt/strand.c), so a longjmp back to it from another
    context lands in a live frame. Returns the number of sites."""
    n = 0
    for f in sorted(gen.glob('funcs_*.c')):
        text = f.read_text(encoding='utf-8')
        new, k = SETJMP_CALL.subn(lambda m: f'{m.group(1)}XRC_SETJMP(rdram, ctx);', text)
        if k:
            f.write_text(new, encoding='utf-8', newline='\n')
            n += k
    return n


def write_alias_c(pack, gen):
    """the game's own second copy of a libultra function (romonly.py: <name>__2,
    in code captured from RDRAM) calls the runtime's <name>"""
    p = pack.dir / 'aliases.txt'
    rows = [l.split() for l in p.read_text(encoding='utf-8').splitlines() if l.strip()] if p.exists() else []
    out = ['/* generated by n64rx: the game\'s own copies of libultra functions, onto the runtime\'s */',
           '#include "recomp.h"']
    for d, b in rows:
        out += [f'void {b}_recomp(uint8_t* rdram, recomp_context* ctx);',
                f'void {d}(uint8_t* rdram, recomp_context* ctx) {{ {b}_recomp(rdram, ctx); }}']
    (gen / 'xrc_alias.c').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    return len(rows)


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
    rp = pack.dir / 'interp_ra.txt'   # romonly.py: routines that read $ra as a value, entered with the caller's
    ra = set(rp.read_text(encoding='utf-8').split()) if rp.exists() else set()
    wp = pack.dir / 'want.txt'   # romonly.py: RDRAM the console should save when this one first runs
    want = {}
    if wp.exists():
        want = {l.split()[0]: l.split()[1:] for l in wp.read_text(encoding='utf-8').splitlines() if l.strip()}
    out = ['/* generated by n64rx: hand-written routines the runtime interprets (PLAN.md 3.14) */',
           '#include "recomp.h"', 'void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);',
           'void xrc_interp_ra(uint8_t* rdram, recomp_context* ctx, uint32_t entry);',
           'void xrc_learn_want(uint32_t lo, uint32_t hi);', '']
    for n, v in rows:
        fn = 'xrc_interp_ra' if n in ra else 'xrc_interp'
        if n in want:
            lo, hi = want[n]
            out.append(f'void {n}(uint8_t* rdram, recomp_context* ctx) {{ static int w; if (!w) {{ w = 1; '
                       f'xrc_learn_want({lo}u, {hi}u); }} {fn}(rdram, ctx, 0x{v:08X}u); }}')
        else:
            out.append(f'void {n}(uint8_t* rdram, recomp_context* ctx) {{ {fn}(rdram, ctx, 0x{v:08X}u); }}')
    if rows:
        out += ['', 'typedef struct { uint32_t vram; recomp_func_t* fn; } XrcInterpEntry;',
                'const XrcInterpEntry xrc_interp_table[] = {']
        out += [f'    {{ 0x{v:08X}u, {n} }},' for n, v in rows]
        out += ['};', f'const unsigned xrc_interp_count = {len(rows)};']
    out += _ultra_table(pack)
    (gen / 'xrc_interp.c').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    return len(rows)


def write_recomp_config(pack, rom_file, gen, cfg_dir):
    text = (pack.dir / 'recomp.toml').read_text(encoding='utf-8')
    text = text.replace('@ROM@', rom_file.resolve().as_posix()).replace('@OUT@', gen.resolve().as_posix())
    text = re.sub(r'^(\s*symbols_file_path\s*=\s*)"([^"]+)"', lambda m: f'{m.group(1)}"{(pack.dir / m.group(2)).resolve().as_posix()}"',
                  text, flags=re.M)
    if os.environ.get('N64RX_TRACE') == '1':   # diagnosis: N64Recomp's trace_mode (xbox/include/xrc/trace.h)
        text = re.sub(r'^\[input\]$', '[input]\ntrace_mode = true', text, count=1, flags=re.M)
    cfg_dir.mkdir(parents=True, exist_ok=True)
    cfg = cfg_dir / 'recomp.toml'
    cfg.write_text(text, encoding='utf-8', newline='\n')
    return cfg


ZERO_DEST = re.compile(r'^(\s*)0 = (.+);$', re.M)


ADDEND = re.compile(r'^(\s*)gpr (jr_addend_[0-9A-Fa-f]+) = 0;$')


def _dedupe_addends(text):
    """the pinned generator writes a jump table's addend declaration with the
    instruction that sets it; when that instruction is a jal's delay slot it
    is written twice ("redefinition of jr_addend_..."): within a function the
    later ones become assignments"""
    out, seen, k = [], set(), 0
    for line in text.split('\n'):
        if line.startswith('RECOMP_FUNC '):
            seen = set()
        m = ADDEND.match(line)
        if m:
            if m.group(2) in seen:
                line = f'{m.group(1)}{m.group(2)} = 0;'
                k += 1
            seen.add(m.group(2))
        out.append(line)
    return '\n'.join(out), k


def fix_zero_dest(gen):
    """a load into $zero (lw $zero, 0($a0): a read for its side effect or a
    probe) comes out of the recompiler as "0 = MEM_W(...);", which C
    rejects: the read is kept, the result dropped (docs/decisions.md)"""
    n = 0
    for c in gen.glob('funcs_*.c'):
        text = c.read_text(encoding='utf-8', errors='replace')
        new, k = ZERO_DEST.subn(r'\1(void)(\2);', text)
        new, k2 = _dedupe_addends(new)   # and a jump table's addend declared twice
        if k or k2:
            c.write_text(new, encoding='utf-8', newline='\n')
            n += k + k2
    return n


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
    write_missing_c(gen, extern)
    return extern


def write_missing_c(gen, extern):
    """a weak body for each of those (xbox/src/rt/ultra/more3.c): a libultra
    internal nothing provides links and names itself at run time; the
    runtime's or a pack's own body replaces it"""
    out = ['/* generated by n64rx: weak bodies for what the generated code calls and nothing may provide',
           ' * (xbox/src/rt/ultra/more3.c; -DXRC_NO_MISSING_STUBS=1 leaves them out) */',
           '#include "recomp.h"', '#if !XRC_NO_MISSING_STUBS',
           'void xrc_missing(const char* name, uint8_t* rdram, recomp_context* ctx);']
    out += [f'__attribute__((weak)) void {n}(uint8_t* rdram, recomp_context* ctx) {{ xrc_missing("{n}", rdram, ctx); }}'
            for n in extern]
    (gen / 'xrc_missing.c').write_text('\n'.join(out + ['#endif', '']), encoding='utf-8', newline='\n')


def _ultra_table(pack):
    """the runtime's libultra (ULTRA(name) in xbox/src/rt) at the game's
    addresses, for the function table: interpreted code (the safety net,
    xbox/src/rt/learn.c) that calls osVirtualToPhysical by address must
    reach the runtime's, not run libultra's own"""
    import tomllib
    have = set()
    for c in (ROOT / 'xbox/src/rt').rglob('*.c'):
        have.update(re.findall(r'^ULTRA\((\w+)\)', c.read_text(encoding='utf-8', errors='replace'), re.M))
    try:
        cfg = tomllib.loads((pack.dir / 'recomp.toml').read_text(encoding='utf-8'))
        sp = pack.dir / cfg['input'].get('symbols_file_path', '')
        secs = tomllib.loads(sp.read_text(encoding='utf-8')).get('section', []) if sp.is_file() else []
    except (OSError, KeyError, tomllib.TOMLDecodeError):
        secs = []
    rows = sorted({(f['vram'], f['name'][:-3] if f['name'].endswith('__2') else f['name'])
                   for s in secs for f in s.get('functions', [])
                   if f['name'] in have or (f['name'].endswith('__2') and f['name'][:-3] in have)})
    out = ['', "/* the runtime's libultra at the game's addresses (xbox/src/rt/ovl.c enters them) */",
           'typedef struct { uint32_t vram; recomp_func_t* fn; } XrcUltraEntry;']
    out += [f'void {n}_recomp(uint8_t* rdram, recomp_context* ctx);' for n in sorted({n for _v, n in rows})]
    out += ['const XrcUltraEntry xrc_ultra_table[] = {'] + [f'    {{ 0x{v:08X}u, {n}_recomp }},' for v, n in rows]
    out += ['    { 0, 0 }', '};', f'const unsigned xrc_ultra_count = {len(rows)};']
    return out


def write_game_c(pack, gpr_bits, gen, cic=6102):
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
void xrc_entry_interp(uint8_t* rdram, recomp_context* ctx);   /* gen/xrc_interp.c, when the entry is interpreted */
{rom_pins_c(pack)}
const XrcGame xrc_game = {{
    .id = {c_str(pack.id)},
    .title = {c_str(pack.title)},
    .title_id = 0x{pack.title_id:08X}u,
    .rdram_mb = {pack.rdram_mb},
    .entrypoint = 0x{pack.entrypoint:08X}u,
    .boot_size = 0x{pack.boot_size:X}u,
    .gpr_bits = {gpr_bits},
    .test_rom = {1 if pack.test else 0},
    .entry = {'xrc_entry_interp' if (pack.dir / 'entry_interp').exists() else 'recomp_entrypoint'},
    .sections = section_table,
    .num_sections = ARRLEN(section_table),
    .icon = xrc_icon_64,
    .code = {c_str(''.join(ch if ' ' <= ch <= '~' else '_' for ch in pack.code))},
    .rom_size = {pack.rom_size}u,
    .sha1 = {{ {''.join(c_str(h) + ', ' for h in pack.sha1[:3])}0 }},
    .eeprom_bytes = {EEPROM.get(pack.save_type, 0)}u,
    .rom_pins = {'rom_pins' if pack.rom_pins else '0'},
    .num_rom_pins = {len(pack.rom_pins)}u,
    .cic = {cic}u,
}};
''', encoding='utf-8', newline='\n')


RESIDENT = {'always': ' -DXRC_ROM_RESIDENT_MB=64', 'never': ' -DXRC_ROM_RESIDENT_MB=0'}


def rom_pins_c(pack):
    """the pack's [rom] pin ranges (offset, length pairs) for the page cache"""
    if not pack.rom_pins:
        return ''
    rows = ', '.join(f'0x{o:X}u, 0x{n:X}u' for o, n in pack.rom_pins)
    return f'static const uint32_t rom_pins[] = {{ {rows} }};\n'


def readme(pack, folder, rom_beside=False, status=''):
    where = ('The ROM next to it is yours,\nnormalised; the game reads it from here and checks its SHA-1.'
             if rom_beside else
             'Your ROM is inside default.xbe\n(normalised); the game reads it from there and checks its SHA-1.')
    return f'''{pack.title} for the original Xbox, built by N64Recomp-X {VERSION}
{status}
Copy this whole folder to your Xbox, for example to
    E:\\Games\\{folder}\\    or    F:\\Games\\{folder}\\
and start default.xbe from your dashboard. {where}

On the console:
  BACK                     the settings menu (video, widescreen, volume, ...)
  hold L + R while loading safe video: 480i 4:3, saved
  hold L + R + START       safe video and default settings
  hold BACK while loading  the settings menu before the game
  L + R + BACK + BLACK     back to the dashboard, anywhere
  L + R + BACK + WHITE     restart, anywhere

Saves, settings and logs: E:\\UDATA\\{pack.title_id:08X}\\
This folder contains your game's code: keep it to yourself.
'''


def _inflate(rom, prog):
    """a Yaz0-compressed ROM rebuilt as the game runs it: every file of its
    DMA table at its vrom, inflated, the table saying "not compressed"
    (PLAN.md 3.5; yaz0.py). The SHA-1 the console checks is the new image's
    ($ROM holds it). N64RX_YAZ0=0 leaves the ROM as it is."""
    import dataclasses
    import hashlib
    from . import yaz0
    try:
        out = yaz0.inflate_rom(rom.data)
    except (ValueError, IndexError, struct.error) as e:   # a corrupt table or block: the ROM as it is (review 3.r2)
        prog.note(f'Yaz0: not inflated ({e}); the ROM is used as it is')
        return rom
    if not out:
        return rom
    img, at, n = out
    prog.note(f'Yaz0: {n} compressed files inflated (the DMA table at 0x{at:X}): {len(rom.data) >> 20} MB -> '
              f'{len(img) >> 20} MB')
    return dataclasses.replace(rom, data=img, sha1=hashlib.sha1(img).hexdigest())


def build(rom_path, out=None, engine="auto", video="auto", fast=False, gpr=None, echo=None, eng=None,
          analyze=False, rom_beside=False, learn=(), events=None):
    echo = echo or (lambda s: print(s, flush=True))
    prog = Progress(echo, 5, events)
    prog.step('Reading the ROM')
    rom = romlib.read(rom_path)
    try:
        pack = packs.match(rom, packs.all_packs())
    except N64rxError:
        pack = None   # no pack: the ROM alone (PLAN.md 3.14)
    rom_sha1 = rom.sha1   # the ROM as supplied (games/learned/ is keyed by it, before a Yaz0 rebuild)
    if pack is None and os.environ.get('N64RX_YAZ0', '1') != '0':
        rom = _inflate(rom, prog)   # Yaz0 games (the Zeldas): the image they run from (yaz0.py)
    eng = eng or engines.pick(engine)
    out_root = pathlib.Path(out) if out else pathlib.Path.cwd() / 'out'
    if eng.name == 'docker':
        return _build_in_docker(eng, rom, out_root, video, fast, gpr, prog, rom_beside, learn, events)
    ainfo = None
    # games/learned/<code>.<sha1>.txt: the facts the project's console rounds found for this exact ROM
    # (tools/xbox/learned_facts.py; addresses and sizes, no bytes), read first. N64RX_LEARNED=0: not read
    shipped = ROOT / 'games' / 'learned' / f'{rom.code}.{rom_sha1[:12]}.txt'
    if pack is None and os.environ.get('N64RX_LEARNED', '1') != '0' and shipped.is_file():
        learn = [shipped] + list(learn)
        prog.note(f'the project\'s learnt facts for this ROM: {shipped.relative_to(ROOT)}')
    learnt = learnmod.read(learn) if learn else None   # --learn: the console's learn.txt (learn.py)
    if analyze or pack is None or learnt is not None:
        base = pack
        bdir = BUILD_ROOT() / ((base.id + '-rom') if base else re.sub(r'[^a-z0-9]', '', rom.code.lower()))
        bdir.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        pack, ainfo = romonly.make(rom, bdir, base, learnt)
        if dryboot.enabled():   # the boot run on this PC first (dryboot.py): its learn file before the user's
            db = dryboot.run(rom, ainfo['analysis'], pack, bdir, prog, bdir / 'dryboot.log')
            if db:
                learn = [db] + list(learn)
                learnt = learnmod.read(learn)
                pack, ainfo = romonly.make(rom, bdir, base, learnt)
        if learnt is not None:
            prog.note(f'learnt on the console: {len(learnt.funcs)} functions, {len(learnt.dmas)} DMAs ({", ".join(map(str, learn))})')
        a = ainfo['analysis']
        prog.note(f'{pack.title}: {rom.describe()}, {rom.order} byte order, '
                  + ('analysed from the ROM over its pack' if base else 'no pack: analysed from the ROM alone'))
        prog.note(f'{len(a.all_funcs())} functions in {len(a.img.segments)} segments, {len(ainfo["ids"])} libultra '
                  f'functions named, in {time.time() - t0:.1f} s ({bdir / "analysis.txt"})')
    else:
        prog.note(f'{pack.title}: {rom.describe()}, {rom.order} byte order, SHA-1 known to the pack')
        bdir = BUILD_ROOT() / pack.id
    bdir.mkdir(parents=True, exist_ok=True)
    log = bdir / 'n64rx.log'
    log.write_text(f'n64rx {VERSION} build {rom.path} ({engines.host_summary()}, engine {eng.name})\n', encoding='utf-8')
    rom_file = bdir / 'rom.z64'
    if not rom_file.exists() or rom_file.read_bytes() != rom.data:
        rom_file.write_bytes(rom.data)
    recomp_rom = rom_file   # what N64Recomp reads: the ROM, plus code learnt from RDRAM (learn.py) past its end
    if ainfo and ainfo.get('recomp_rom'):
        recomp_rom = bdir / 'rom_learnt.z64'
        if not recomp_rom.exists() or recomp_rom.read_bytes() != ainfo['recomp_rom']:
            recomp_rom.write_bytes(ainfo['recomp_rom'])

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
        for _attempt in range(400):   # one refused function a run (SM64 ROM-only: ~100, jal into a segment found only at run time)
            cfg = write_recomp_config(pack, recomp_rom, gen, bdir / 'cfg')
            start = log.stat().st_size
            if not eng.recompile(cfg, log) and (gen / 'funcs.h').exists():
                break
            refused = _refused(log, start)
            if not ainfo or not refused or not _ignore_dead(pack, ainfo, refused, prog):
                raise N64rxError('N64Recomp failed on this ROM.', f'the log says why:\n{tail(log)}\nfull log: {log}')
            if gen.exists():
                shutil.rmtree(gen)
        else:
            raise N64rxError('N64Recomp refused 400 functions, one a run, and still failed.',
                             'send the log to the project (the analyser should have marked them up front):\n'
                             f'{tail(log)}\nfull log: {log}')
        if fix_zero_dest(gen):
            prog.note('loads into $zero kept as reads')
        extern = write_extern_h(gen)
        ninterp = write_interp_c(pack, gen)
        nsj = write_setjmp_sites(gen)
        if nsj:
            prog.note(f'{nsj} calls to the game\'s setjmp made host setjmps (coroutines: strand.c)')
        nguard = write_guard(pack, gen, recomp_rom.read_bytes())
        if os.environ.get('N64RX_SP_CHECK', '0') != '0':
            prog.note(f'{write_sp_check(gen)} calls check $sp on return (N64RX_SP_CHECK, trace.c)')
        if nguard:
            prog.note(f'{nguard} functions from captured code checked against RDRAM before they run (guard.c)')
        nfodd = write_fodd(gen, saves.thread_fr(recomp_rom.read_bytes()))
        if nfodd:
            prog.note(f'{nfodd} odd-FPR accesses follow Status.FR: the game\'s threads run with it (XRC_FR)')
        nres = write_restrict(gen)   # last: the rewrites above match the plain signature
        if nres:
            prog.note(f'{nres} functions take restrict pointers (N64RX_RESTRICT)')
        if write_alias_c(pack, gen):
            prog.note("the game's own copies of libultra go to the runtime's (gen/xrc_alias.c)")
        if ninterp:
            prog.note(f"{ninterp} hand-written routines run interpreted")
        prog.note(f'{len(extern)} functions the runtime or the pack provides')
        bits = decide_gpr(pack, rom, gpr, prog, recomp_rom.read_bytes())
        write_game_c(pack, bits, gen, rom.cic)
        (gen / '.n64rx-bits').write_text(str(bits))
        stamp.write_text(key)
    bits = int((gen / '.n64rx-bits').read_text())
    nfunc = (gen / 'funcs.h').read_text().count('(uint8_t* rdram, recomp_context* ctx);')
    nfiles = len(list(gen.glob('funcs_*.c')))
    prog.note(f'{nfunc} functions in {nfiles} files, {bits}-bit registers')

    prog.step(f'Compiling for the Xbox ({nfiles} generated files + the runtime{", fast" if fast else ", ThinLTO"})')
    flags = ['-DXRC_GPR32=1'] if bits == 32 else []
    flags += saves.cflags(pack, rom.data)   # libultra globals the runtime's saves keep (saves.py)
    recomp_bytes = recomp_rom.read_bytes()
    flags += saves.consistent_threads(saves.running_thread(pack, recomp_bytes),   # __osRunningThread, kept by thread.c
                                      saves.faulted_thread(recomp_bytes))          # libultra's fault path (tlb.c)
    flags += saves.os_exception(recomp_bytes, ainfo['analysis'].img.segments if ainfo else ())   # its exception code, for a TLB miss (interp.c)
    flags += saves.os_runqueue(recomp_bytes)            # its run queue, a sentinel while that code runs
    fr = saves.thread_fr(recomp_bytes)                   # the float mode its osCreateThread gives (recomp.h XRC_FR)
    flags += [f'-DXRC_FR={fr}'] if fr else []
    flags += learnmod.rdram_user(learnt)                 # RDRAM's home when KUSEG lands on the kernel's (mem.c)
    flags += learnmod.high_tlb(learnt)                   # KSEG2/3 through the game's TLB (recomp.h, tlb.c)
    env = {
        'XBOX_BUILD_DIR': engines.msys_path(bdir / 'xbox') if engines.IS_WINDOWS else str(bdir / 'xbox'),
        'XRC_GEN_DIR': engines.msys_path(gen) if engines.IS_WINDOWS else str(gen),
        'XRC_TITLE_ID': pack.title_id_hex,
        # cxbe's title goes through CMake's post-build shell line unquoted ("Cruis'n USA" broke it);
        # xbe_sections.py writes the real one afterwards
        'XRC_XBE_TITLE': re.sub(r'[^A-Za-z0-9 ._+!-]', '', pack.title).strip() or 'N64Recomp-X',
        'XRC_GAME_CFLAGS': ' '.join(flags),
        # N64RX_CFLAGS: a developer's extra flags (a test build: -DXHW_AUTOPAD=1), docs/testing.md
        'XBOX_CFLAGS': f'-DXRC_VIDEO_DEFAULT={VIDEO[video]}' + (' -O1' if fast else '')
                       # [rom] resident: always | never (the page cache for every size); auto: by size (PLAN.md 3.5)
                       + RESIDENT.get(pack.raw.get('rom', {}).get('resident', 'auto'), '')
                       + (' ' + os.environ['N64RX_CFLAGS'] if os.environ.get('N64RX_CFLAGS') else ''),
        'XBOX_LTO': '0' if fast else '1',
    }
    if eng.build_xbe(env, log) or not (bdir / 'xbox/xbe/default.xbe').exists():
        raise N64rxError('Compiling or linking the XBE failed.', f'the log says why:\n{tail(log)}\nfull log: {log}')

    prog.step('Finishing the XBE (title ID, icon' + (')' if rom_beside else ', the ROM inside)'))
    xbe = bdir / 'xbox/xbe/default.xbe'
    folder = output_folder(out_root, folder_name(pack.title))
    shutil.copy2(xbe, folder / 'default.xbe')
    r = subprocess.run([sys.executable, str(ROOT / 'tools/xbox/xbe_sections.py'), str(folder / 'default.xbe'),
                        '--title-id', pack.title_id_hex, f'--name={pack.title}', '--icon', str(pack.icon),
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
    status = compat.line(rom.code, rom.version)   # its tier (docs/compat.md)
    (folder / 'README.txt').write_text(readme(pack, folder.name, rom_beside, status + '\n'), encoding='utf-8',
                                       newline='\r\n')
    shutil.copy2(bdir / 'xbox/n64recomp_x.map', folder / 'debug/n64recomp_x.map')
    prog.done()
    (cache_dir() / 'last.json').write_text(json.dumps({'pack': pack.id, 'log': str(log), 'folder': str(folder),
                                                        'time': time.time()}))
    if events:
        events({'ev': 'result', 'folder': str(folder), 'status': status})
        return folder
    echo(f'\nDone: {folder}')
    echo(status)
    echo(f'Copy that folder to your Xbox (E:\\Games\\{folder.name}\\) and start default.xbe.')
    return folder


def _build_in_docker(eng, rom, out_root, video, fast, gpr, prog, rom_beside=False, learn=(), events=None):
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
    for i, f in enumerate(learn):   # the learn files ride in the output folder (mounted at /out)
        shutil.copyfile(f, out_root / f'.learn-{i}.txt')
        args += ['--learn', f'/out/.learn-{i}.txt']
    prog.note(f'in the SDK image {engines.image_name()} (the first build pulls it)')
    if eng.run_inside(args, rom.path.resolve().parent, out_root.resolve(), log):
        raise N64rxError('The build inside Docker failed.', f'{tail(log)}\nfull log: {log}')
    prog.done()
    if events:   # the folder the run inside made: the newest one with an XBE
        made = sorted((p for p in out_root.iterdir() if (p / 'default.xbe').exists()), key=lambda p: p.stat().st_mtime)
        events({'ev': 'result', 'folder': str(made[-1] if made else out_root),
                'status': compat.line(rom.code, rom.version)})
    return out_root
