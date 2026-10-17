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

LINE = re.compile(r'^(?:.*\[LEARN\]\s+)?(func|dma)\s+([0-9A-Fa-fx]+)\s+([0-9A-Fa-fx]+)(?:\s+(?:from\s+)?([0-9A-Fa-fx]+))?\s*$')


@dataclass
class Learnt:
    funcs: dict = field(default_factory=dict)   # vram -> (bytes seen, caller)
    dmas: list = field(default_factory=list)    # (rom offset, ram, size), file order
    code: dict = field(default_factory=dict)    # block vram -> its bytes, from RDRAM (no ROM DMA put them there)

    def __bool__(self):
        return bool(self.funcs or self.dmas or self.code)


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
            if kind == 'func' and x & 3 == 0 and 0x80000000 <= x < 0x80800000:
                n, c = out.funcs.get(x, (0, 0))
                out.funcs[x] = (max(n, y), c or (int(z, 16) if z else 0))
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
    for x in sorted(learnt.funcs):
        d = next((d for d in reversed(learnt.dmas) if d[1] <= x < d[1] + d[2]), None)
        # where overlays share the address, the one the newest DMA put there
        seg = next((s for s in a.img.segments if s.has(x) and d and s.rom - s.vram == d[0] - d[1]), None) or \
            next((s for s in a.img.segments if s.has(x) and not d), None)
        if seg is None and d is None:   # code no ROM DMA put there: the bytes the console saved
            blk = next((v for v in learnt.code if v <= x < v + len(learnt.code[v])), None)
            if blk is not None:
                name = f'code_{blk:08X}'
                seg = next((s for s in a.img.segments if s.name == name), None)
                if seg is None:
                    data = learnt.code[blk]
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
        roots = [x for x, f in have.items() if f.how != 'gap'] + new
        a.funcs[name] = F.discover(a.img, seg, roots)
        notes.append(f'{name}: {len(new)} learnt functions ({", ".join(f"{x:08X}" for x in new[:8])}'
                     f'{"..." if len(new) > 8 else ""}), {len(a.funcs[name])} functions now')
    a.notes += notes
    return notes


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
