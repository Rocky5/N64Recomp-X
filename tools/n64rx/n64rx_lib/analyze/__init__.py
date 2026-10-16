"""The ROM analyser (PLAN.md 3.14): any ROM in, what N64Recomp needs out,
with no decomp. analyze(rom) finds the segments and their functions; the
libultra rules (libultra.py) name the functions the runtime replaces; emit.py
writes the symbol file. Everything is derived on the user's machine from
their ROM; nothing of it is committed.

Overlays (PLAN.md 3.4): segments may share vram (a game's levels loaded
one at a time at the same address from different ROM ranges). A function
is keyed by its vram when no other segment covers that range, and by
vram | ROM offset << 32 when its segment overlaps another (key() / vram()),
so the same address in two overlays is two functions with two names."""
from dataclasses import dataclass, field

from . import funcs as F
from . import segments as S


class AnalysisError(Exception):
    pass


@dataclass
class Analysis:
    entry: int
    main: int
    bss: int
    img: F.Image
    funcs: dict = field(default_factory=dict)      # segment name -> {start: Func}
    notes: list = field(default_factory=list)

    def overlays(self):
        """the names of the segments whose vram another segment shares"""
        segs = self.img.segments
        return {s.name for s in segs for o in segs if o is not s and
                s.vram < o.vram + o.size and o.vram < s.vram + s.size}

    def key(self, seg, x, ovl=None):
        """the function at vram x of seg, as all_funcs() keys it"""
        if seg.name in (self.overlays() if ovl is None else ovl):
            return x | (seg.rom + x - seg.vram) << 32
        return x

    def all_funcs(self):
        ovl = self.overlays()
        segs = {s.name: s for s in self.img.segments}
        return {self.key(segs[n], a, ovl) if n in segs else a: f for n, fs in self.funcs.items() for a, f in fs.items()}

    def by_vram(self, funcs=None):
        """vram -> [keys]: every overlay's function at that address"""
        out = {}
        for k in (funcs if funcs is not None else self.all_funcs()):
            out.setdefault(k & 0xFFFFFFFF, []).append(k)
        return out

    def seg_of(self, k):
        """the segment a key belongs to"""
        x, r = k & 0xFFFFFFFF, k >> 32
        for s in self.img.segments:
            if s.has(x) and (not r or s.rom + x - s.vram == r) and x in self.funcs.get(s.name, {}):
                return s
        return None


def vram(k):
    return k & 0xFFFFFFFF


def analyze(rom, extra_segments=()):
    """rom: the whole ROM in z64 order. extra_segments: Segments learnt from
    earlier runs (the learning loop)."""
    entry = int.from_bytes(rom[8:12], 'big')
    e = F.parse_entry(rom, entry)
    if not e:
        raise AnalysisError(f'the entry code at {entry:08X} is not the SDK\'s (no BSS clear and jump to main)')
    # the 6103 and 6106 boot chips' IPL3 loads 1 or 2 MB below the header's
    # entrypoint: the entry code's own addresses (the BSS, main) say where it runs
    for off in (0, 0x100000, 0x200000):
        e2 = F.parse_entry(rom, entry - off)
        if e2 and entry - off < e2[0] <= entry - off + 0x100000 and entry - off <= e2[2] < e2[0]:
            entry, e = entry - off, e2
            break
    bss, bss_size, main = e
    img = F.Image(rom)
    boot = F.Segment('boot', 0x1000, entry, bss - entry)
    if not 0 < boot.size <= 0x100000:
        raise AnalysisError(f'the boot segment would be {boot.size:#x} bytes (BSS at {bss:08X})')
    img.segments.append(boot)
    a = Analysis(entry, main, bss, img)
    a.funcs['boot'] = F.discover(img, boot, [entry, main])
    _end_entry(a, rom)
    a.notes.append(f'boot: ROM 0x1000 at {entry:08X}, {boot.size:#x} bytes (BSS {bss:08X} +{bss_size:#x}), '
                   f'main {main:08X}, {len(a.funcs["boot"])} functions')
    for seg in extra_segments:
        _add(a, seg, [seg.vram])
    while True:
        new = S.find(img, list(a.funcs.values()), len(rom))
        if not new:
            break
        for seg, inside, pc in new:
            _add(a, seg, inside, S.weak(seg, inside))
            src = f'the DMA at {pc:08X}' if isinstance(pc, int) else pc
            a.notes.append(f'{seg.name}: ROM {seg.rom:#x} at {seg.vram:08X}, {seg.size:#x} bytes ({src}; '
                           f'called at {", ".join(f"{t:08X}" for t in inside[:3])}), '
                           f'{len(a.funcs[seg.name])} functions')
    return a


def _end_entry(a, rom):
    """an entry that calls main with jal (parse_entry) ends after the call
    and the spin loop after it, if any: what follows is data (strings, the
    linker's padding) a walk would take for code"""
    from .mips import Ins
    ef = a.funcs['boot'].get(a.entry)
    if ef is None:
        return
    for i in range(48):
        pc = a.entry + 4 * i
        ins = Ins(int.from_bytes(rom[0x1000 + 4 * i:0x1004 + 4 * i], 'big'), pc)
        if ins.is_jr or ins.is_j:
            return
        if ins.is_jal and ins.target == a.main:
            end = pc + 8
            nxt = Ins(int.from_bytes(rom[0x1000 + 4 * i + 8:0x1000 + 4 * i + 12], 'big'), end)
            if nxt.is_branch and nxt.is_unconditional_branch and nxt.branch_target <= end:
                end += 8   # b . and its delay slot
            if end < ef.end:
                ef.end = end
            return


def _add(a, seg, roots, weak=()):
    a.img.segments.append(seg)
    a.funcs[seg.name] = F.discover(a.img, seg, list(roots), weak)
