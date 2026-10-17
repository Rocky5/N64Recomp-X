"""The learning loop (PLAN.md 3.14 step 5): code the analyser did not find
still runs on the console (the runtime's safety net interprets it,
xbox/src/rt/learn.c) and is written down in E:\\UDATA\\<title id>\\learn.txt:

    func <vram> <bytes> from <caller>     a function that ran interpreted
    dma <rom offset> <ram> <size>         the PI DMA that put code there

(hex; the console's serial log has the same lines after "[LEARN] ", and a
log works as the input too). `n64rx build <rom> --learn <file>` reads it
and the analysis takes those functions as roots and those DMAs as
segments, so the next build recompiles them. What is learnt stays in the
user's build folder (pack/learned.txt), like the rest of the analysis.

N64RX_WITHHOLD (a test switch: comma-separated addresses) takes functions
out of the analysis, as if the analyser had missed them, so the net and
this loop can be shown on any game; a function the learn file names is
never withheld."""
import os
import pathlib
import re
from dataclasses import dataclass, field

from .analyze import funcs as F

LINE = re.compile(r'^(?:.*\[LEARN\]\s+)?(func|dma|kuseg)\s+([0-9A-Fa-fx]+)\s+([0-9A-Fa-fx]+)(?:\s+(?:from\s+)?([0-9A-Fa-fx]+))?\s*$')


@dataclass
class Learnt:
    funcs: dict = field(default_factory=dict)   # vram -> (bytes seen, caller)
    dmas: list = field(default_factory=list)    # (rom offset, ram, size), file order
    code: dict = field(default_factory=dict)    # block vram -> its bytes, from RDRAM (no ROM DMA put them there)
    kuseg: dict = field(default_factory=dict)   # 1 MB KUSEG regions the TLB mapped -> 1 if the host could not alias one
    high: set = field(default_factory=set)      # 1 MB KSEG2/3 regions the TLB mapped (the host never can: XRC_HIGH_TLB)

    def __bool__(self):
        return bool(self.funcs or self.dmas or self.code or self.kuseg or self.high)


def _code_records(data, out):
    """learn_code.bin: records of (vram, size) big-endian, then the bytes"""
    i = 0
    while i + 8 <= len(data):
        v = int.from_bytes(data[i:i + 4], 'big')
        n = int.from_bytes(data[i + 4:i + 8], 'big')
        if not n or i + 8 + n > len(data):
            break
        out.code.setdefault(v, bytes(data[i + 8:i + 8 + n]))
        i += 8 + n


def _code_from_log(text, out):
    """a test build's serial log: [LEARNCODE] BEGIN n, base64 lines, END"""
    import base64
    buf, inside = [], False
    for line in text.splitlines():
        j = line.find('[LEARNCODE] ')
        if j < 0:
            continue
        rest = line[j + 12:].strip()
        if rest.startswith('BEGIN'):
            buf, inside = [], True
        elif rest == 'END':
            if inside:
                _code_records(base64.b64decode(''.join(buf)), out)
            inside = False
        elif inside:
            buf.append(rest)


def read(paths):
    """the learn files (or serial logs) at `paths`, merged"""
    out = Learnt()
    for p in paths:
        p = pathlib.Path(p)
        side = p.with_name('learn_code.bin')   # the console's: beside learn.txt
        if side.is_file():
            _code_records(side.read_bytes(), out)
        text = p.read_text(encoding='utf-8', errors='replace')
        _code_from_log(text, out)
        for line in text.splitlines():
            m = LINE.match(line.strip())
            if not m:
                continue
            kind, x, y, z = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            if kind == 'func' and x & 3 == 0 and not 0x80800000 <= x < 0xC0000000:   # KSEG0, or TLB-mapped
                n, c = out.funcs.get(x, (0, 0))
                out.funcs[x] = (max(n, y), c or (int(z, 16) if z else 0))
            elif kind == 'kuseg' and x < 0x80000000:
                out.kuseg[x & ~0xFFFFF] = out.kuseg.get(x & ~0xFFFFF, 0) | (y & 1)
            elif kind == 'kuseg' and x >= 0xC0000000:
                out.high.add(x & ~0xFFFFF)
            elif kind == 'dma' and z and 0 < int(z, 16) <= 0x800000:
                d = (x, (y & 0x1FFFFFFF) | 0x80000000, int(z, 16))
                if d not in out.dmas:
                    out.dmas.append(d)
    return out


def write(learnt, path):
    lines = ['# learnt on the console (n64rx --learn): the safety net ran these (PLAN.md 3.14)']
    lines += [f'dma {r:08x} {v:08x} {s:x}' for r, v, s in learnt.dmas]
    lines += [f'func {x:08x} {n:x} from {c:08x}' for x, (n, c) in sorted(learnt.funcs.items())]
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def withheld(learnt):
    v = os.environ.get('N64RX_WITHHOLD', '')
    out = set()
    for t in re.split(r'[,\s]+', v.strip()):
        if t:
            x = int(t[5:] if t.startswith('func_') else t, 16)
            if x not in learnt.funcs:
                out.add(x)
    return out


def apply(a, learnt, rom_len):
    """the learnt functions into the analysis `a`: a function inside a known
    segment becomes a root of that segment (found again with it); one
    outside every segment brings in the newest DMA that covered it as a new
    segment. Returns the notes for the report."""
    notes = []
    todo = {}
    runs = _code_runs(learnt.code)
    for x in sorted(learnt.funcs):
        d = next((d for d in reversed(learnt.dmas) if d[1] <= x < d[1] + d[2]), None)
        # where overlays share the address, the one the newest DMA put there
        seg = next((s for s in a.img.segments if s.has(x) and d and s.rom - s.vram == d[0] - d[1]), None) or \
            next((s for s in a.img.segments if s.has(x) and not d), None)
        if seg is None and d is None:   # code no ROM DMA put there: the bytes the console saved
            blk = next((v for v in runs if v <= x < v + len(runs[v])), None)
            if blk is not None:
                name = f'code_{blk:08X}'
                seg = next((s for s in a.img.segments if s.name == name), None)
                if seg is None:
                    data = runs[blk]
                    off = (len(a.img.rom) + 0xF) & ~0xF   # appended to the build's copy of the ROM
                    a.img.rom = bytes(a.img.rom) + bytes(off - len(a.img.rom)) + data
                    seg = F.Segment(name, off, blk, len(data))
                    a.img.segments.append(seg)
                    a.funcs[seg.name] = {}
                    notes.append(f'{name}: {len(data):#x} bytes at {blk:08X} from RDRAM (learnt: no ROM DMA put '
                                 f'them there; the recompiler reads them past the ROM\'s end, at {off:#x})')
        if seg is None:
            if d is None or d[0] + d[2] > rom_len:
                notes.append(f'learnt {x:08X}: no segment and no DMA covers it (skipped)')
                continue
            name = f'learn_{d[1]:08X}'
            if any(s.name == name for s in a.img.segments):
                name += f'_{d[0]:X}'
            seg = F.Segment(name, d[0], d[1], d[2])
            a.img.segments.append(seg)
            a.funcs[seg.name] = {}
            notes.append(f'{seg.name}: ROM {d[0]:#x} at {d[1]:08X}, {d[2]:#x} bytes (learnt: a DMA the console saw)')
        todo.setdefault(seg.name, (seg, []))[1].append(x)
    for name, (seg, xs) in todo.items():
        have = a.funcs.get(name, {})
        new = [x for x in xs if x not in have]
        if not new:
            continue
        captured = name.startswith('code_')   # one overlay's bytes: calls into it are weak (funcs.discover)
        roots = [x for x, f in have.items() if f.how != 'gap' and (f.how == 'entry' or not captured)] + new
        a.funcs[name] = F.discover(a.img, seg, roots, weak_calls=captured)
        notes.append(f'{name}: {len(new)} learnt functions ({", ".join(f"{x:08X}" for x in new[:8])}'
                     f'{"..." if len(new) > 8 else ""}), {len(a.funcs[name])} functions now')
    _cross_roots(a, notes)
    _table_sources(a, runs, notes)
    a.notes += notes
    return notes


def _table_sources(a, runs, notes):
    """captured KSEG0 RDRAM over a ROM segment whose bytes differ there (a
    boot that decompresses its code and data over its own segment: GoldenEye,
    whose 0x70000000 code reads switch tables at 0x8002xxxx). Appended to the
    recompiler's copy of the ROM as sections with no functions, written ahead
    of the others (emit.write_syms), so N64Recomp with patches/recomp/0001
    reads a jump table outside its function's section from what RDRAM held.
    Kept out of a.img.segments: no overlay keys or function searches change."""
    a.table_sources = getattr(a, 'table_sources', [])
    rom = a.img.rom
    for blk, data in sorted(runs.items()):
        if not 0x80000000 <= blk < 0x80800000:
            continue
        over = [s for s in a.img.segments if not s.name.startswith('code_') and s.vram < blk + len(data) and blk < s.vram + s.size]
        differ = False
        for s in over:
            lo, hi = max(blk, s.vram), min(blk + len(data), s.vram + s.size)
            r = s.rom + (lo - s.vram)
            if r + (hi - lo) > len(rom) or rom[r:r + (hi - lo)] != data[lo - blk:hi - blk]:
                differ = True
                break
        if not differ:
            continue
        off = (len(rom) + 0xF) & ~0xF
        rom = bytes(rom) + bytes(off - len(rom)) + data
        a.table_sources.append(F.Segment(f'rdram_{blk:08X}', off, blk, len(data)))
        notes.append(f'rdram_{blk:08X}: {len(data):#x} bytes at {blk:08X} from RDRAM, unlike the ROM segment '
                     f'there (decompressed at boot): read for jump tables only, at {off:#x}')
    a.img.rom = rom


def _code_runs(code):
    """captured blocks that follow each other in RDRAM, joined: one segment
    each, so a function crossing a page boundary is whole. Records overlap
    (64 KB blocks from older consoles' captures, 4 KB pages from newer):
    the smaller one's bytes win (a page is saved when code in it first runs,
    a block could hold parts not loaded yet)."""
    pages = {}
    for v in sorted(code, key=lambda v: -len(code[v])):
        b = code[v]
        for off in range(0, len(b) - 0xFFF, 0x1000):
            pages[(v + off) & ~0xFFF] = b[off:off + 0x1000]
    runs, cur, end = {}, None, None
    for vp in sorted(pages):
        if cur is not None and vp == end:
            runs[cur] += pages[vp]
        else:
            cur = vp
            runs[vp] = pages[vp]
        end = vp + 0x1000
    return runs


def _cross_roots(a, notes):
    """a call from one captured segment into another where no function starts
    yet (Banjo-Kazooie: ~150 of them; N64Recomp refuses each caller, "No
    function found for jal target"): a root of the segment that holds it,
    until nothing new turns up"""
    code = [s for s in a.img.segments if s.name.startswith('code_')]
    if not code:
        return
    added = 0
    for _round in range(8):
        want = {}
        for seg in a.img.segments:
            for f in a.funcs.get(seg.name, {}).values():
                for t in f.calls:
                    if seg.has(t):
                        continue
                    holds = [c for c in code if c.has(t)]
                    if len(holds) == 1 and t not in a.funcs.get(holds[0].name, {}):
                        want.setdefault(holds[0].name, (holds[0], set()))[1].add(t)
        if not want:
            break
        for name, (seg, xs) in want.items():
            have = a.funcs.get(name, {})
            a.funcs[name] = F.discover(a.img, seg, [x for x, f in have.items() if f.how == 'entry'] + sorted(xs),
                                       weak=sorted(xs), weak_calls=True)
            added += len(xs)
    if added:
        notes.append(f'captured code: {added} functions called from another captured segment found')


def withhold(a, xs):
    """N64RX_WITHHOLD: these starts out of the analysis (the net's test)"""
    gone = []
    for fs in a.funcs.values():
        for x in list(fs):
            if x in xs:
                del fs[x]
                gone.append(x)
    if gone:
        a.notes.append(f'withheld (N64RX_WITHHOLD, the safety net\'s test): {", ".join(f"{x:08X}" for x in gone)}')
    return gone


def extend_live(a, live, starts, nm, replaced):
    """the learnt functions are live (the console ran them), with what they call"""
    funcs = a.all_funcs()
    bv = a.by_vram(funcs)
    todo = [k for x in starts for k in bv.get(x, ())]   # an overlay address: each overlay's function
    # code captured from RDRAM is the game's own, reached mostly through pointers the analysis
    # cannot follow: every function found in it is live
    for seg in a.img.segments:
        if seg.name.startswith('code_'):
            todo += [k for k in a.funcs.get(seg.name, {}) if k in funcs]
    before = len(live)
    while todo:
        x = todo.pop()
        if x in live:
            continue
        live.add(x)
        if nm.get(x) in replaced:
            continue
        todo += [t for v in funcs[x].calls | funcs[x].tails for t in bv.get(v, ()) if t not in live]
    a.notes.append(f'learnt on the console: {len(starts)} functions, {len(live) - before} of them (with what they '
                   f'call) live only through it')
    return live


def high_tlb(learnt):
    """-DXRC_HIGH_TLB=1 when the game mapped KSEG2/3 (Paper Mario's effects at
    0xE0200000): rdram + vaddr is the Xbox kernel's system space there, so
    the generated code translates such addresses through the game's TLB
    (recomp.h XRC_ADDR, rt/tlb.c). N64RX_HIGH_TLB=0/1 overrides."""
    env = os.environ.get('N64RX_HIGH_TLB')
    if env is not None:
        return ['-DXRC_HIGH_TLB=1'] if env == '1' else []
    return ['-DXRC_HIGH_TLB=1'] if learnt and learnt.high else []


def rdram_user(learnt):
    """-DXRC_RDRAM_USER=<va>: where the runtime puts RDRAM's KSEG0 when the
    game maps KUSEG pages whose host address (rdram + vaddr) would land on
    the kernel's map (rt/mem.c): rdram = va - 0x80000000, chosen so the
    lowest such region lands at 16 MB (above the XBE) and every region up to
    0x80000000 in user space below KSEG0; KSEG1 then sits 512 MB above."""
    if not learnt or not any(learnt.kuseg.values()):
        return []
    low = min(learnt.kuseg)
    va = (0x01000000 - low + 0x80000000) & 0xFFFFFFFF
    if not 0x01000000 < va < 0x5F000000:   # KSEG1's alias must stay in user space too
        return []
    return [f'-DXRC_RDRAM_USER=0x{va:08X}u']
