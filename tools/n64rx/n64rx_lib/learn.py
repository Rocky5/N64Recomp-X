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

RDRAM8 = re.compile(r'^(?:.*\[LEARN\]\s+)?rdram\s+8\s*$')
KUSEG_MULTI = re.compile(r'^(?:.*\[LEARN\]\s+)?kuseg_versions\s+([0-9A-Fa-f]+)\s*$')
RELOC = re.compile(r'^(?:.*\[LEARN\]\s+)?reloc\s+([0-9A-Fa-f]+)(?:\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+))?\s*$')   # an overlay the game relocated (zovl.py): its link vram, where, how long
LINE = re.compile(r'^(?:.*\[LEARN\]\s+)?(func|dma|kuseg)\s+([0-9A-Fa-fx]+)\s+([0-9A-Fa-fx]+)(?:\s+(?:from\s+)?([0-9A-Fa-fx]+))?\s*$')


@dataclass
class Learnt:
    funcs: dict = field(default_factory=dict)   # vram -> (bytes seen, caller)
    dmas: list = field(default_factory=list)    # (rom offset, ram, size), file order
    code: dict = field(default_factory=dict)    # block vram -> its bytes, from RDRAM (no ROM DMA put them there)
    kuseg: dict = field(default_factory=dict)   # 1 MB KUSEG regions the TLB mapped -> 1 if the host could not alias one
    rdram_mb: int = 0   # 8: the TLB mapped physical memory past 4 MB (the Expansion Pak's; rt/tlb.c)
    high: set = field(default_factory=set)      # 1 MB KSEG2/3 regions the TLB mapped (the host never can: XRC_HIGH_TLB)
    alts: dict = field(default_factory=dict)    # page vram -> other code seen there, most often first (_choose)
    seen: dict = field(default_factory=dict)    # page vram -> {bytes: [times, first, page record]}
    kuseg_multi: set = field(default_factory=set)   # TLB-mapped pages that held other code at other times
    reloc: set = field(default_factory=set)         # link addresses of overlays the game relocated (zovl.py)
    reloc_at: set = field(default_factory=set)      # (ram, length) where it relocated them

    def __bool__(self):
        return bool(self.funcs or self.dmas or self.code or self.kuseg or self.high or self.rdram_mb or self.reloc)


# kill switches: N64RX_LEARN_MAJORITY=0 keeps the first capture of a page (as before); N64RX_CODE_VERSIONS=0
# keeps one version of captured code where the game loads several at one address (Mario Tennis's scenes)
MAJORITY = os.environ.get('N64RX_LEARN_MAJORITY', '1') != '0'
VERSIONS = os.environ.get('N64RX_CODE_VERSIONS', '1') != '0'
ALT_MIN = 256    # words of 1024 a page's other version differs in to count as other code (not data beside it)
ALT_MAX = 3      # versions kept beside the chosen one


def _code_records(data, out):
    """learn_code.bin: records of (vram, size) big-endian, then the bytes"""
    i = 0
    while i + 8 <= len(data):
        v = int.from_bytes(data[i:i + 4], 'big')
        n = int.from_bytes(data[i + 4:i + 8], 'big')
        if not n or i + 8 + n > len(data):
            break
        b = bytes(data[i + 8:i + 8 + n])
        if MAJORITY and not (v & 0xFFF) and not (n & 0xFFF):   # pages: every version counted (_choose)
            for off in range(0, n, 0x1000):
                e = out.seen.setdefault(v + off, {}).setdefault(b[off:off + 0x1000], [0, len(out.seen), n == 0x1000])
                e[0] += 1
        else:
            out.code.setdefault(v, b)
        i += 8 + n


def _choose(out):
    """each captured page's bytes: a page record over a block's (a page is saved when code in it first runs,
    a block could hold parts not loaded yet), then the version seen most often, then the first seen (one
    round can catch a page mid-load: Mario Tennis's 0x80042000 in round 1 against six later rounds). The
    other page versions that differ in ALT_MIN words or more are kept (alts): the game loads other code
    there at other times"""
    for v, vs in out.seen.items():
        order = sorted(vs.items(), key=lambda kv: (-kv[1][2], -kv[1][0], kv[1][1]))
        best = order[0][0]
        out.code[v] = best
        alt = []
        for b, (_n, _first, page) in order[1:]:
            if not page or len(alt) >= ALT_MAX:
                continue
            if sum(1 for k in range(0, 0x1000, 4) if b[k:k + 4] != best[k:k + 4]) >= ALT_MIN and                     all(sum(1 for k in range(0, 0x1000, 4) if b[k:k + 4] != c[k:k + 4]) >= ALT_MIN for c in alt):
                alt.append(b)
        if alt and VERSIONS:
            out.alts[v] = alt


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
            m = KUSEG_MULTI.match(line.strip())
            if m:
                out.kuseg_multi.add(int(m.group(1), 16) & ~0xFFF)
                continue
            m = RELOC.match(line.strip())
            if m:
                out.reloc.add(int(m.group(1), 16))
                if m.group(2):
                    out.reloc_at.add((int(m.group(2), 16), int(m.group(3), 16)))
                continue
            if RDRAM8.match(line.strip()):
                out.rdram_mb = 8
                continue
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
    _choose(out)
    _drop_kuseg_multi(out)
    return out


# kill switch: N64RX_KUSEG_MULTI=0 keeps a TLB-mapped page that held other code at other times
KUSEG_MULTI_DROP = os.environ.get('N64RX_KUSEG_MULTI', '1') != '0'


def _drop_kuseg_multi(out):
    """a page the game maps through its TLB that held other code at other times (the console said so, or
    two captures differ: Castlevania: Legacy of Darkness pages each actor's code in at 0x0F000000) is left to
    the safety net, its functions too: one actor's bytes recompiled ran for every actor there (a call through
    a NULL pointer). ROM-backed pages are found from the ROM (_rom_backed) and are not captures"""
    if not KUSEG_MULTI_DROP:
        return
    # a page that differs in a quarter of its words or more (alts): the console's own flag alone also
    # fires on a page whose data changes beside its code (Acclaim's 0x00100000 is RDRAM 0, the vectors)
    multi = {p for p in out.alts if p < 0x80000000}
    if not multi:
        return
    regions = {p & ~0xFFFFF for p in multi}   # the whole 1 MB region: an overlay spans several pages
    for p in [p for p in out.code if p < 0x80000000 and (p & ~0xFFFFF) in regions]:
        out.code.pop(p, None)
        out.alts.pop(p, None)
    for x in [x for x in out.funcs if x < 0x80000000 and (x & ~0xFFFFF) in regions]:
        del out.funcs[x]


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
    runs = _code_runs(_without_relocated(learnt, notes))
    runs = _rom_backed(a, runs, rom_len, notes)
    _table_sources(a, runs, notes)   # before the walks: they read jump tables there
    runs, spans = _version_spans(runs, learnt.alts)
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
        # a start learnt where the game loads other code at other times (Mario Tennis's scenes) may be
        # another version's: a root only where these bytes look like one starts, else weak
        weak = [x for x in new if not _plausible(a.img.rom, seg.rom + x - seg.vram)] if captured and STARTS else []
        a.funcs[name] = F.discover(a.img, seg, roots, weak=weak, weak_calls=captured)
        notes.append(f'{name}: {len(new)} learnt functions ({", ".join(f"{x:08X}" for x in new[:8])}'
                     f'{"..." if len(new) > 8 else ""}), {len(a.funcs[name])} functions now')
    _versions(a, learnt, runs, spans, notes)
    _cross_roots(a, notes)
    a.notes += notes
    return notes


# kill switch: N64RX_LEARN_STARTS=0 makes every learnt start in captured code a root (as before)
STARTS = os.environ.get('N64RX_LEARN_STARTS', '1') != '0'


def _plausible(data, off):
    """whether a function can start at `off` of these bytes: a frame opened there (addiu sp, sp, -n), or the
    code before it ends one (jr ra or a j with its delay slot, then any padding)"""
    def w(o):
        return int.from_bytes(data[o:o + 4], 'big') if 0 <= o and o + 4 <= len(data) else None
    x = w(off)
    if not x:
        return False
    if x >> 16 == 0x27BD and x & 0x8000:
        return True
    o = off - 4
    while o >= 0 and w(o) == 0:   # padding, or the nop in the delay slot
        o -= 4
    if o < 0:
        return True
    return any(y is not None and (y == 0x03E00008 or y >> 26 == 2) for y in (w(o), w(o - 4)))


def _version_spans(runs, alts):
    """the runs holding pages with other code at other times (alts): each becomes overlays (_versions),
    whole, so no function is cut at a span's edge; runs without such pages (a game's libultra) keep plain
    keys and names"""
    spans = [(blk, blk + len(data)) for blk, data in sorted(runs.items())
             if any(p in alts for p in range(blk, blk + len(data), 0x1000))]
    return runs, spans


def _versions(a, learnt, runs, spans, notes):
    """each other version of a span's code its own segment at the same vram (an overlay, keyed by the ROM
    offset it is appended at, as Pokemon Snap's levels are): pages with no other version keep the chosen
    bytes. Found from the chosen version's starts and the learnt functions there, all weak (another
    version's starts). The runtime's guard runs the version whose bytes RDRAM holds (rt/guard.c)"""
    for lo, hi in spans:
        seg0 = next((s for s in a.img.segments if s.name == f'code_{lo:08X}'), None)
        if seg0 is None:
            continue
        base = runs[lo]
        cands = sorted(set(a.funcs.get(seg0.name, {})) | {x for x in learnt.funcs if lo <= x < hi})
        for j in range(1, ALT_MAX + 1):
            data = b''.join(learnt.alts[p][j - 1] if len(learnt.alts.get(p, ())) >= j else base[p - lo:p - lo + 0x1000]
                            for p in range(lo, hi, 0x1000))
            if data == base:
                break
            off = (len(a.img.rom) + 0xF) & ~0xF
            a.img.rom = bytes(a.img.rom) + bytes(off - len(a.img.rom)) + data
            seg = F.Segment(f'code_{lo:08X}_v{j}', off, lo, len(data))
            a.img.segments.append(seg)
            roots = [x for x in cands if _plausible(data, x - lo)]   # this version's starts, not another's
            a.funcs[seg.name] = F.discover(a.img, seg, roots, weak=roots, weak_calls=True)
            notes.append(f'{seg.name}: another version of the code at {lo:08X} ({len(data):#x} bytes, captured at other '
                         f'times: the game loads other code there), {len(a.funcs[seg.name])} functions, at {off:#x}')


def _rom_backed(a, runs, rom_len, notes):
    """captured KUSEG pages the ROM holds as they are: code a game pages in
    through its TLB straight from the ROM (GoldenEye's 0x7F000000, romCopy
    from its game segment's ROM start + the page's offset, tlb_manage.c in
    its decomp). Pages that agree on one ROM offset for their vram become one
    ROM-backed segment, from its 1 MB base to the last such page and 64 KB
    on, so the whole of it is found from the ROM rather than a page a
    learning round. Returns the runs left for capture segments.
    N64RX_TLB_ROM=0: every page stays a capture, as before."""
    if os.environ.get('N64RX_TLB_ROM', '1') == '0':
        return runs
    rom = a.img.rom
    pages, captured = {}, set()
    for blk, data in runs.items():
        for off in range(0, len(data), 0x1000):
            v, b = blk + off, data[off:off + 0x1000]
            if v >= 0x80000000 or len(b) < 0x1000 or b.count(b[:4]) * 4 == len(b):
                continue
            captured.add(v)
            r = rom.find(b[:64], 0, rom_len)
            while r != -1 and rom[r:r + 0x1000] != b:
                r = rom.find(b[:64], r + 1, rom_len)
            if r != -1:
                pages[v] = r - v
    by = {}
    for v, d in pages.items():
        by.setdefault((v & 0xFFF00000, d), []).append(v)
    taken = set()
    for (base, d), vs in sorted(by.items()):
        if base + d < 0:
            continue
        end = min(max(vs) + 0x1000 + 0x10000, base + 0x100000, rom_len - d)
        # never over a captured page of the region that differs from the ROM at that offset: Perfect
        # Dark's first two pages at 0x70000000 match the ROM's start, the rest of its code there does
        # not (0x70003EE0 compiled from unrelated ROM bytes). N64RX_TLB_ROM_RUN=0: as before
        lo = base
        if os.environ.get('N64RX_TLB_ROM_RUN', '1') != '0':
            other = [v for v in captured if base <= v < base + 0x100000 and pages.get(v) != d]
            lo = max([v + 0x1000 for v in other if v < min(vs)], default=base)
            end = min([end] + [v for v in other if v > min(vs)])
        if end <= lo or any(s.has(lo) or s.has(end - 1) for s in a.img.segments):
            continue
        name = f'tlb_{base:08X}' if lo == base else f'tlb_{lo:08X}'
        seg = F.Segment(name, lo + d, lo, end - lo)
        a.img.segments.append(seg)
        a.funcs[seg.name] = {}
        taken.update(range(lo, end, 0x1000))
        notes.append(f'{seg.name}: ROM {lo + d:#x} at {lo:08X}, {end - lo:#x} bytes (learnt: {len(vs)} pages the '
                     f'game mapped through its TLB match the ROM there)')
    if not taken:
        return runs
    left = {}
    for blk, data in runs.items():   # what no ROM-backed segment covers stays a capture
        cur = None
        for off in range(0, len(data), 0x1000):
            v = blk + off
            if v in taken:
                cur = None
                continue
            if cur is None:
                cur = v
                left[cur] = b''
            left[cur] += data[off:off + 0x1000]
    return left


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
    # the walker reads jump tables there too (funcs.Image.word): GoldenEye's 0x7F000000 functions with
    # their tables at 0x8005xxxx then recompile (its level's interpreted instructions 8.4 million per 300
    # retraces -> 42 thousand, 26 -> 30 fps in xemu). Its black level and modelless watch with this on
    # were its cosf falling into sinf (emit._size), not the tables. N64RX_WALK_RDRAM=0: off
    if os.environ.get('N64RX_WALK_RDRAM', '1') == '1':
        a.img.table_sources = a.table_sources


def _without_relocated(learnt, notes):
    """the captured code less the pages wholly inside a place the game relocated an overlay to
    (`reloc` lines): that was the overlay's code, which the build now recompiles from the ROM as a
    relocatable section (analyze/zovl.py); kept, it was a second copy at one address, and Ocarina of
    Time's image the bigger for it. N64RX_RELOC_CAPTURES=1 keeps them"""
    if not learnt.reloc_at or os.environ.get('N64RX_RELOC_CAPTURES', '0') == '1':
        return learnt.code
    spans = sorted(learnt.reloc_at)
    out, dropped = {}, 0
    for v, b in learnt.code.items():
        keep = bytearray()
        start = None
        for off in range(0, len(b), 0x1000):
            p = (v + off) & 0x1FFFFFFF
            inside = any(r & 0x1FFFFFFF <= p and p + 0x1000 <= (r & 0x1FFFFFFF) + n for r, n in spans) and v >= 0x80000000
            if inside:
                dropped += 1
                if start is not None:
                    out[start] = bytes(keep)
                    keep, start = bytearray(), None
                continue
            if start is None:
                start = v + off
            keep += b[off:off + 0x1000]
        if start is not None and keep:
            out[start] = bytes(keep)
    if dropped:
        notes.append(f'{dropped} captured pages left out: where the game relocated an overlay this build recompiles')
    return out


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
    code = [s for s in a.img.segments if s.name.startswith(('code_', 'tlb_'))]
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
    ovl = a.overlays()
    for seg in a.img.segments:
        if seg.name.startswith(('code_', 'tlb_')):   # tlb_: the same code, found in the ROM (_rom_backed)
            # keyed as all_funcs keys them (an overlay's with its ROM offset: the versions, _versions)
            todo += [a.key(seg, k, ovl) for k in a.funcs.get(seg.name, {}) if a.key(seg, k, ovl) in funcs]
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
    # and the span's host range held at boot, before anything else is put there (rt/boot.c)
    end = max(learnt.kuseg) + 0x100000
    return [f'-DXRC_RDRAM_USER=0x{va:08X}u', f'-DXRC_KUSEG_HOLD=0x{low:08X}u', f'-DXRC_KUSEG_HOLD_END=0x{end:08X}u']
