#!/usr/bin/env python3
"""What the analyser hands the interpreter before N64Recomp runs
(analyze/emit.py recomp_hazards) and how captured RDRAM blocks join
(learn.py _code_runs), on hand-assembled words: no ROM. The jump-table
finder mirrors N64Recomp's analysis.cpp: lui, an addu of the index, lw, jr."""
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib.analyze import emit  # noqa: E402
from n64rx_lib import learn  # noqa: E402

AT, T6, T7, A0, SP, ZERO = 1, 14, 15, 4, 29, 0
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f'test_precheck: FAIL {what}')


def i_(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def r_(rs, rt, rd, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | funct


def lui(rt, imm): return i_(15, 0, rt, imm)
def addiu(rt, rs, imm): return i_(9, rs, rt, imm)
def addu(rd, rs, rt): return r_(rs, rt, rd, 33)
def lw(rt, imm, base): return i_(35, base, rt, imm)
def sw(rt, imm, base): return i_(43, base, rt, imm)
def jr(rs): return r_(rs, 0, 0, 8)
NOP = 0

# the usual shape: lui at, hi; addu at, at, index; lw t6, lo(at); jr t6
ws = [lui(AT, 0x8010), addu(AT, AT, T6), lw(T6, 0x1234, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80101234], 'lui/addu/lw/jr: the table at 0x80101234')

# a negative lo: hi 0x8011 + (-0x10)
ws = [lui(AT, 0x8011), addu(AT, T6, AT), lw(T6, -0x10, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x8010FFF0], 'a negative lo')

# addiu of the lo before the addu, lw with offset 0
ws = [lui(AT, 0x8010), addiu(AT, AT, 0x40), addu(AT, AT, T6), lw(T6, 0, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100040], 'addiu then lw 0')

# through the stack: sw then lw from sp keeps the state
ws = [lui(AT, 0x8010), addu(T7, AT, T6), sw(T7, 0x18, SP), lw(T7, 0x18, SP), lw(T6, 0x20, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100020], 'kept across the stack')

# a move (or rd, rs, $zero) keeps it; an addu of two lui registers loses it, as in N64Recomp
ws = [lui(AT, 0x8010), r_(AT, ZERO, T7, 37), addu(T7, T7, T6), lw(T6, 0, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100000], 'or keeps the lui')
ws = [lui(AT, 0x8010), lui(T7, 0x8020), addu(T7, T7, AT), lw(T6, 0, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'addu of two lui registers')

# jr $ra is a return, not a table
ws = [lui(AT, 0x8010), addu(AT, AT, T6), lw(31, 0, AT), jr(31), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'jr ra')

# a write in between clears the register
ws = [lui(AT, 0x8010), addu(AT, AT, T6), i_(13, 0, AT, 5), lw(T6, 0, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'ori overwrites the base')

# captured blocks: neighbours join, a gap starts a new run
code = {0x80280000: b'a' * 0x10000, 0x80290000: b'b' * 0x10000, 0x802B0000: b'c' * 0x10000}
runs = learn._code_runs(code)
check(sorted(runs) == [0x80280000, 0x802B0000], f'runs: {sorted(hex(x) for x in runs)}')
check(len(runs[0x80280000]) == 0x20000 and runs[0x80280000][0x10000:0x10001] == b'b', 'the joined run holds both blocks')

# a game's own setjmp/longjmp by shape (romonly._setjmp_longjmp; strand.c runs them as coroutines)
import struct  # noqa: E402
import tempfile  # noqa: E402
import types  # noqa: E402
from n64rx_lib import romonly, pipeline  # noqa: E402
REGS = (29, 31, 16, 17, 18, 19, 20, 21, 22, 23, 30)
sj = [sw(r, 4 * i, A0) for i, r in enumerate(REGS)] + [i_(57, A0, 20 + k, 0x2C + 4 * k) for k in range(12)] + \
    [jr(31), addu(2, 0, 0)]
lj = [lw(r, 4 * i, A0) for i, r in enumerate(REGS)] + [i_(49, A0, 20 + k, 0x2C + 4 * k) for k in range(12)] + \
    [addu(2, 5, 0), jr(31), NOP]
rom = struct.pack(f'>{len(sj) + len(lj)}I', *(sj + lj))
seg = types.SimpleNamespace(name='main', rom=0, vram=0x80000400)
F = types.SimpleNamespace
funcs = {0x80000400: F(start=0x80000400, end=0x80000400 + 4 * len(sj)),
         0x80000400 + 4 * len(sj): F(start=0x80000400 + 4 * len(sj), end=0x80000400 + len(rom))}
an = F(img=F(segments=[seg], rom=rom), funcs={'main': funcs})
check(romonly._setjmp_longjmp(an) == {0x80000400: 'n64_setjmp', 0x80000400 + 4 * len(sj): 'n64_longjmp'},
      'setjmp and longjmp by shape')
an.funcs['main'] = {0x80000400: funcs[0x80000400]}
check(romonly._setjmp_longjmp(an) == {}, 'a setjmp alone is not a pair')

with tempfile.TemporaryDirectory() as d:
    g = pathlib.Path(d)
    (g / 'funcs_0.c').write_text('void f(uint8_t* rdram, recomp_context* ctx) {\n    n64_setjmp(rdram, ctx);\n'
                                 '    n64_setjmp_x(rdram, ctx);\n}\n', encoding='utf-8')
    check(pipeline.write_setjmp_sites(g) == 1, 'one setjmp site')
    t = (g / 'funcs_0.c').read_text(encoding='utf-8')
    check('    XRC_SETJMP(rdram, ctx);\n' in t and 'n64_setjmp_x(rdram, ctx);' in t, 'the call made XRC_SETJMP')

# a bal into another function's body (Resident Evil 2's captured code): N64Recomp would make a static there
# whose branches past its end are labels it lacks, and the routine reads ra as a value: the caller is interpreted
RA_ = 31
fa = [i_(1, 0, 17, 5), NOP, jr(RA_), NOP]   # 0x80001000: bal 0x80001018 (into fb's body)
fb = [NOP, NOP, NOP, jr(RA_), NOP, NOP]     # 0x80001010
rom = struct.pack(f'>{len(fa) + len(fb)}I', *(fa + fb))
seg = F(name='code_80001000', rom=0, vram=0x80001000, has=lambda t: 0x80001000 <= t < 0x80001000 + len(rom))
funcs = {0x80001000: F(start=0x80001000, end=0x80001010), 0x80001010: F(start=0x80001010, end=0x80001028)}
an = F(img=F(segments=[seg], rom=rom), funcs={seg.name: funcs}, entry=0x80000400,
       key=lambda s, v, o: v, overlays=lambda: {})
live = set(funcs)
nm = {x: f'func_{x:08X}' for x in funcs}
look = set()
check(emit.cross_funcs(an, live, nm, set(), look) == {0x80001000: 'func_80001000'} and look == set(),
      f'bal into a body: the caller interpreted ({look})')
fa[0] = i_(1, 0, 17, 3)   # bal 0x80001010: fb's start, an ordinary call
an.img.rom = struct.pack(f'>{len(fa) + len(fb)}I', *(fa + fb))
look = set()
check(emit.cross_funcs(an, live, nm, set(), look) == {} and look == set(), 'bal to a start: nothing')

# code a game decompressed over its own ROM segment (Turok: Dinosaur Hunter): the build's copy takes RDRAM's
# bytes where the learnt starts look like starts only there; not where the ROM's look like code too
SP_ = 29
frame = [addiu(SP_, SP_, -0x18), NOP, jr(RA_), addiu(SP_, SP_, 0x18)]
code = struct.pack('>8I', *(frame + frame))                  # two functions, at +0 and +0x10
packed = bytes(range(0x11, 0x31))                             # "compressed": no frame, no return
rom = b'\x55' * 0x1000 + packed + b'\0' * 0x20               # nothing before it ends a function either
seg = F(name='boot', rom=0x1000, vram=0x80001000, size=0x40)
lt = learn.Learnt()
lt.funcs = {0x80001000: 1, 0x80001010: 1}
an = F(img=F(segments=[seg], rom=rom), funcs={'boot': {}})
sp = learn._decompressed_code(an, lt, {0x80001000: code}, len(rom), [])
check(sp == {'boot': [(0x80001000, 0x80001020)]} and an.img.rom[0x1000:0x1020] == code, f'decompressed code taken: {sp}')
an = F(img=F(segments=[seg], rom=b'\0' * 0x1000 + code + b'\0' * 0x20), funcs={'boot': {}})
other = struct.pack('>8I', *([NOP] * 4 + frame))              # the ROM's code is code too: an overlay over it
sp = learn._decompressed_code(an, lt, {0x80001000: other}, len(an.img.rom), [])
check(sp == {} and an.img.rom[0x1000:0x1020] == code, 'plausible ROM code kept')
lt.dmas = [(0x2000, 0x80001000, 0x20)]
an = F(img=F(segments=[seg], rom=rom), funcs={'boot': {}})
check(learn._decompressed_code(an, lt, {0x80001000: code}, len(rom), []) == {}, 'a logged DMA there: kept')

# an Expansion Pak probe by hand (Armorines: bump the word at 0x80400000, read it back): 8 MB
S0, S1 = 16, 17
probe_ws = [lui(S1, 0x8040), lw(S0, 0, S1), addiu(S0, S0, 1), sw(S0, 0, S1), jr(RA_), NOP]
rom = struct.pack(f'>{len(probe_ws)}I', *probe_ws)
seg = F(name='main', rom=0, vram=0x80001000, size=len(rom), has=lambda t: 0x80001000 <= t < 0x80001000 + len(rom))
an = F(img=F(segments=[seg], rom=rom), funcs={'main': {0x80001000: F(start=0x80001000, end=0x80001000 + len(rom))}},
       entry=0x80009000)
check(romonly._wants_8mb(an), 'a probe of 0x80400000: 8 MB')
os.environ['N64RX_PROBE_8MB'] = '0'
check(not romonly._wants_8mb(an), 'N64RX_PROBE_8MB=0: 4 MB')
del os.environ['N64RX_PROBE_8MB']
probe_ws[3] = NOP   # a load alone (a heap's end compared, not a probe)
an.img.rom = struct.pack(f'>{len(probe_ws)}I', *probe_ws)
check(not romonly._wants_8mb(an), 'a load alone: 4 MB')

# a main that maps the boot code with its own tlbwi and jumps there (Conker): its target and base
from n64rx_lib.analyze import funcs as AF  # noqa: E402
T0_, T1_ = 8, 9
def ori(rt, rs, imm): return i_(13, rs, rt, imm)
def mtc0(rt, rd): return (16 << 26) | (4 << 21) | (rt << 16) | (rd << 11)
main_ws = [lui(T0_, 0x007F), ori(T0_, T0_, 0xE000), mtc0(T0_, 5),     # PageMask 4 MB pages
           lui(T0_, 0x1000), mtc0(T0_, 10),                            # EntryHi 0x10000000
           ori(T0_, 0, 0x1F), mtc0(T0_, 2),                            # EntryLo0 PFN 0, valid
           lui(T1_, 1), ori(T1_, T1_, 0x1F), mtc0(T1_, 3),             # EntryLo1 PFN 0x400
           0x42000002,                                                  # tlbwi
           lui(T0_, 0x1000), addiu(T0_, T0_, 0x1050), jr(T0_), NOP]
rom = b'\0' * 0x1000 + b'\0' * 0x40 + struct.pack(f'>{len(main_ws)}I', *main_ws) + b'\0' * 0x200
check(AF.tlb_main(rom, 0x80001000, 0x80001040) == (0x10001050, 0x10000000), 'tlb_main: target and base')
main_ws[11:13] = [lui(T0_, 0x2000), addiu(T0_, T0_, 0x1050)]          # a jump outside the mapping's image
rom = b'\0' * 0x1040 + struct.pack(f'>{len(main_ws)}I', *main_ws) + b'\0' * 0x200
check(AF.tlb_main(rom, 0x80001000, 0x80001040) is None, 'tlb_main: a jump elsewhere')

# a KSEG2/3 TLB mapping on the console (Paper Mario's effects): the next build translates it (XRC_HIGH_TLB)
with tempfile.TemporaryDirectory() as d:
    lg = pathlib.Path(d) / 'run.log'
    lg.write_text('[LEARN] kuseg e0200000 1\n[LEARN] kuseg 7f000000 0\n', encoding='utf-8')
    lt = learn.read([lg])
    check(lt.high == {0xE0200000} and 0x7F000000 in lt.kuseg, 'kuseg lines: KSEG2/3 apart from KUSEG')
    check(learn.high_tlb(lt) == ['-DXRC_HIGH_TLB=1'] and learn.high_tlb(learn.Learnt()) == [], 'XRC_HIGH_TLB from them')

print('test_precheck: ' + ('ok' if not fails else f'{fails} failed'))
sys.exit(1 if fails else 0)
