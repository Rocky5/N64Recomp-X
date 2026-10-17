"""A game pack made from the ROM alone (PLAN.md 3.14): the analyser's
segments, functions and libultra names as N64Recomp's symbol file and
config, in the build folder (never in the tree). A real pack, when there
is one, goes on top: its instruction patches and ignored functions are
mapped onto the analysis by address.

Without a pack: the title is the header's, the title ID 0x4E52 and a
16-bit hash of the game code (0x0100 and up: below is kept for packs), the
save type from SAVE_TYPES (public facts per game code, the emulators'
kind) or 4 Kbit EEPROM when the game probes for one."""
import hashlib
import os
import pathlib
import re
import tomllib

from . import ROOT, N64rxError
from .packs import Pack
from .analyze import AnalysisError, analyze
from .analyze import emit, libultra
from . import learn

# 16 Kbit EEPROM games by game code (first three letters); the rest of the
# EEPROM games are 4 Kbit. Facts, not ROM bytes.
EEP16K = {'NPW', 'NYS', 'NB7', 'NFU', 'NGT', 'NPD', 'NMV', 'NRZ', 'NUB', 'ND6', 'NDO', 'NEP', 'NEV', 'N3D', 'NCW'}


def _title_id(code):
    h = int.from_bytes(hashlib.sha1(code.encode('ascii', 'replace')).digest()[:2], 'big')
    if h < 0x0100 or h == 0xFFFE:
        h = (h | 0x0100) & 0xFFFD
    return 0x4E520000 | h


def make(rom, bdir, base=None, learnt=None):
    """(Pack, info) for this ROM from the analysis; info has the live set and
    the names for the pipeline's retry. learnt: what the console's safety
    net ran (learn.py, --learn)"""
    learnt = learnt or learn.Learnt()
    try:
        a = analyze(rom.data)
    except AnalysisError as e:
        raise N64rxError(f'This ROM could not be analysed: {e}.',
                         'It may use a boot or entry code n64rx does not know yet; '
                         'tell the project which game it is.')
    learn.withhold(a, learn.withheld(learnt))   # the learning loop (learn.py): its test switch,
    if learnt:                                  # then what the console learnt
        learn.apply(a, learnt, len(rom.data))
    # libultra is linked into the boot segment and its like, never into an
    # overlay: the overlays' functions (keys past 32 bits) are not named
    ids, _N = libultra.identify(a.img, {k: f for k, f in a.all_funcs().items() if k < 1 << 32})
    ids.pop(a.entry, None)   # the entry code is the game's own, whatever it looks like (South Park's clears the TLB)
    ids.pop(a.main, None)    # and so is main (Conker's and GoldenEye's set up the TLB, osMapTLBRdb's shape)
    lists = emit.recomp_lists(ROOT)
    replaced = lists['reimplemented_funcs'] | lists['ignored_funcs']
    # code captured from RDRAM (learn.py) carries the game's own libultra, a second copy beside the boot
    # loader's (Rare's games): named on its own, a name the loader's copy has becomes <name>__2, left out of
    # the recompile and given a body that calls the runtime's (gen/xrc_alias.c, pipeline.py)
    aliases = {}
    code_keys = {k for seg in a.img.segments if seg.name.startswith('code_') for k in a.funcs.get(seg.name, {})}
    if code_keys:
        have = _runtime_ultra()
        ids2, _N2 = libultra.identify(a.img, {k: f for k, f in a.all_funcs().items() if k in code_keys})
        taken = {n for n, _w in ids.values()}
        for k, (n, why) in ids2.items():
            if k in ids or k == a.entry:
                continue
            if n not in taken:
                ids[k] = (n, why + " (the game's own copy)")
                taken.add(n)
            elif n in have and f'{n}__2' not in taken:
                ids[k] = (f'{n}__2', why + " (the game's own copy, beside the loader's)")
                aliases[f'{n}__2'] = n
                taken.add(f'{n}__2')
        replaced = replaced | set(aliases)
    nm = emit.names(a, ids)
    live = emit.reachable(a, nm, replaced)
    pdir = pathlib.Path(bdir) / 'pack'
    pdir.mkdir(parents=True, exist_ok=True)
    if learnt:
        live = learn.extend_live(a, live, learnt.funcs, nm, replaced)
        learn.write(learnt, pdir / 'learned.txt')
    emit.write_syms(a, nm, pdir / 'syms.toml')
    ignored, patches = _from_base(base, a, nm)
    # dead code is not recompiled: libultra's internals that only the
    # replaced functions reached (they call registers and each other)
    ignored |= {n for x, n in nm.items() if x not in live and n not in replaced}
    ignored |= set(aliases)   # the runtime's, through gen/xrc_alias.c
    (pdir / 'aliases.txt').write_text(''.join(f'{d} {b}\n' for d, b in sorted(aliases.items())), encoding='utf-8')
    # hand-written code the recompiler cannot express (a return through a
    # register other than ra, a jump through a register with no table): the
    # runtime's interpreter runs it (xbox/src/rt/interp.c)
    interp = {x: nm[x] for x, f in a.all_funcs().items() if x in live and x != a.entry and f.bad.startswith('jr $')}
    # code that reaches the RCP's registers through KSEG1: interpreted, onto
    # the runtime's register model (N64RX_MMIO_INTERP=0: recompiled, as before)
    if os.environ.get('N64RX_MMIO_INTERP', '1') != '0':
        interp.update({x: n for x, n in emit.hw_funcs(a, live, nm, replaced).items() if n not in ignored})
    # a branch into another function's body cannot be compiled: interpreted
    # (N64RX_XBRANCH_INTERP=0: recompiled, the compile then fails as before)
    if os.environ.get('N64RX_XBRANCH_INTERP', '1') != '0':
        interp.update({x: n for x, n in emit.cross_funcs(a, live, nm, replaced).items() if n not in ignored})
    # what N64Recomp would crash on (a jump table outside the function's segment) or refuse one run at a
    # time (a jal to no function): interpreted up front (N64RX_RECOMP_PRECHECK=0: left to the recompiler)
    if os.environ.get('N64RX_RECOMP_PRECHECK', '1') != '0':
        hz = {x: v for x, v in emit.recomp_hazards(a, live, nm, replaced).items() if v[0] not in ignored and x != a.entry}
        interp.update({x: n for x, (n, _why, _want) in hz.items()})
        if hz:
            a.notes.append(f'{len(hz)} functions the recompiler would refuse or crash on, interpreted: ' +
                           ', '.join(f'{n} ({why})' for n, why, _w in list(hz.values())[:6]) + ('...' if len(hz) > 6 else ''))
        # captured code whose jump table is in RDRAM no capture holds yet: its stub asks the console to save
        # the blocks from the code to the table (learn.c xrc_learn_want), for the next --learn round
        (pdir / 'want.txt').write_text(''.join(f'{n} 0x{w[0]:08X} 0x{w[1]:08X}\n' for n, _why, w in hz.values() if w),
                                       encoding='utf-8')
    ignored |= set(interp.values())
    (pdir / 'interp.txt').write_text(''.join(f'{n} 0x{x:08X}\n' for x, n in sorted(interp.items())),
                                     encoding='utf-8')
    cfg = ['# Generated by n64rx from the ROM alone (PLAN.md 3.14).', '[input]',
           f'entrypoint = 0x{a.entry:08X}', 'symbols_file_path = "syms.toml"', 'rom_file_path = "@ROM@"',
           'output_func_path = "@OUT@"', 'recomp_include = "#include \\"recomp.h\\""',
           'functions_per_output_file = 50', '', '[patches]',
           'ignored = [' + ', '.join(f'"{n}"' for n in sorted(ignored)) + ']', '']
    for func, vram, value, why in patches:
        cfg += [f'# {why}', '[[patches.instruction]]', f'func = "{func}"', f'vram = 0x{vram:08X}',
                f'value = 0x{value:08X}', '']
    (pdir / 'recomp.toml').write_text('\n'.join(cfg), encoding='utf-8', newline='\n')
    (pathlib.Path(bdir) / 'analysis.txt').write_text(emit.report(a, ids, live, nm, replaced), encoding='utf-8')
    uses_eep = any(n in ('osEepromProbe', 'osEepromRead', 'osEepromLongRead') for n, _w in ids.values())
    if base:
        p = Pack(**{**base.__dict__, 'dir': pdir, 'id': base.id + '-rom'})
    else:
        code = rom.code
        p = Pack(dir=pdir, id=re.sub(r'[^a-z0-9]', '', code.lower()) or 'game', title=rom.title.title(), code=code,
                 title_id=_title_id(code), sha1=[rom.sha1], rom_size=rom.size, entrypoint=a.entry,
                 boot_size=0x100000, rdram_mb=4, gpr='auto', compressed='none',
                 save_type=('eep16k' if code[:3] in EEP16K else 'eep4k') if uses_eep else 'none', stage='planned',
                 test=False, raw={})
    info = {'live': live, 'names': nm, 'analysis': a, 'ids': ids,
            'recomp_rom': a.img.rom if len(a.img.rom) != len(rom.data) else None}   # learnt code appended
    return p, info


def _runtime_ultra():
    """the libultra entry points the runtime provides: ULTRA(name) in xbox/src/rt"""
    have = set()
    for c in (ROOT / 'xbox/src/rt').rglob('*.c'):
        have.update(re.findall(r'^ULTRA\((\w+)\)', c.read_text(encoding='utf-8', errors='replace'), re.M))
    return have


def _from_base(base, a, nm):
    """a real pack's ignored functions and instruction patches, by address"""
    if not base:
        return set(), []
    cfg = tomllib.loads((base.dir / 'recomp.toml').read_text(encoding='utf-8'))
    syms = {}
    sp = base.dir / cfg['input'].get('symbols_file_path', '')
    if sp.is_file():
        for s in tomllib.loads(sp.read_text(encoding='utf-8')).get('section', []):
            for f in s.get('functions', []):
                syms[f['name']] = f['vram']
    funcs = a.all_funcs()

    def owner(v):
        for x, f in funcs.items():
            if f.start <= v < f.end:
                return x
        return None
    ignored = set()
    for n in cfg.get('patches', {}).get('ignored', []):
        v = syms.get(n)
        if v is None and n.startswith('func_'):
            v = int(n[5:], 16)
        x = owner(v) if v is not None else None
        if x is not None:
            ignored.add(nm[x])
    patches = []
    for p in cfg.get('patches', {}).get('instruction', []):
        x = owner(p['vram'])
        if x is not None:
            patches.append((nm[x], p['vram'], p['value'], f'from the pack: {p["func"]}'))
    return ignored, patches
