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

    def __bool__(self):
        return bool(self.funcs or self.dmas)


def read(paths):
    """the learn files (or serial logs) at `paths`, merged"""
    out = Learnt()
    for p in paths:
        for line in pathlib.Path(p).read_text(encoding='utf-8', errors='replace').splitlines():
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
        seg = next((s for s in a.img.segments if s.has(x)), None)
        if seg is None:
            d = next((d for d in reversed(learnt.dmas) if d[1] <= x < d[1] + d[2]), None)
            if d is None or d[0] + d[2] > rom_len:
                notes.append(f'learnt {x:08X}: no segment and no DMA covers it (skipped)')
                continue
            seg = F.Segment(f'learn_{d[1]:08X}', d[0], d[1], d[2])
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
    todo = [x for x in starts if x in funcs]
    before = len(live)
    while todo:
        x = todo.pop()
        if x in live:
            continue
        live.add(x)
        if nm.get(x) in replaced:
            continue
        todo += [t for t in funcs[x].calls | funcs[x].tails if t in funcs and t not in live]
    a.notes.append(f'learnt on the console: {len(starts)} functions, {len(live) - before} of them (with what they '
                   f'call) live only through it')
    return live
