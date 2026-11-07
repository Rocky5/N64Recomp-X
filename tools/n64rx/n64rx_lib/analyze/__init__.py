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
import os
import zlib
from dataclasses import dataclass, field

from . import funcs as F
from . import segments as S

BOOT_PLAIN = os.environ.get('N64RX_BOOT_PLAIN', '1') != '0'   # Analysis.overlays
# A captured segment whose page starts inside the boot segment's last page
# (NHL 99: code_80005000 against the boot's 0x80000400..0x800051D0, 0x1D0
# bytes, zeros in both) shares no code with it: keyed as an overlay, its
# 2,979 functions got ROM-offset keys, the libultra rules (which match calls
# by vram) named nothing there, the game's own osCreateViManager /
# osViSetEvent / osStartThread ran unreplaced, its own VI manager waited for
# interrupts the runtime never raises, and no retrace reached it. An overlap
# with the boot that holds the same bytes in both and no function start of
# either is not an overlay. N64RX_BOOT_TWIN=0: any overlap makes one, as before
BOOT_TWIN = os.environ.get('N64RX_BOOT_TWIN', '1') != '0'
# the TLB template's second shape: an SDK entry whose main maps the boot code
# with an inline tlbwi and jumps into the mapping (funcs.tlb_main: Conker).
# Analysed at KSEG0, every function the game runs at 0x1000xxxx was unknown
# to the function table (each call through the safety net), libultra was
# named at addresses never called, and the game's own osStartThread's eret
# ended the boot ("The boot code came back"). N64RX_TLB_MAIN=0: KSEG0, as before
TLB_MAIN = os.environ.get('N64RX_TLB_MAIN', '1') != '0'


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

    def _twin_overlap(self, s, o):
        """s and o hold the same bytes where they overlap, and no function of either starts there"""
        lo, hi = max(s.vram, o.vram), min(s.vram + s.size, o.vram + o.size)
        rom = self.img.rom
        if rom[s.rom + lo - s.vram:s.rom + hi - s.vram] != rom[o.rom + lo - o.vram:o.rom + hi - o.vram]:
            return False
        return not any(lo <= x < hi for n in (s.name, o.name) for x in self.funcs.get(n, {}))

    def overlays(self):
        """the names of the segments whose vram another segment shares; the
        boot segment is never one (its functions keep their plain keys and
        names): Paper Mario DMAs code into part of its own (a learnt
        segment at 0x8006C000), and keyed as an overlay the boot lost its
        libultra names and the recompiler its ignore list.
        N64RX_BOOT_PLAIN=0: the boot an overlay like any other"""
        segs = self.img.segments
        out = {s.name for s in segs for o in segs if o is not s and
               s.vram < o.vram + o.size and o.vram < s.vram + s.size and
               not (BOOT_TWIN and 'boot' in (s.name, o.name) and self._twin_overlap(s, o))}
        if BOOT_PLAIN:
            out.discard('boot')
        return out

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
    # the 7102 boot chip's IPL3 (the PAL twin of 6101) loads ROM 0x1000 at 0x80000480 whatever the header
    # says: Lylat Wars' says 0x80000400, and read there its code's jal targets fell 0x80 past their
    # functions (3 functions found; at 0x80000480, 5316 jals land on prologues). N64RX_IPL3_7102=0: the header's
    if zlib.crc32(rom[0x40:0x1000]) == 0x009E9EA3 and os.environ.get('N64RX_IPL3_7102', '1') != '0':
        entry = 0x80000480
    t = F.tlb_entry(rom, entry)   # first: its BSS clear and call to the TLB writer look like main otherwise
    if t:
        return _analyze_tlb(rom, entry, t, extra_segments)
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
    tm = F.tlb_main(rom, entry, main) if TLB_MAIN else None
    if not tm:
        return _analyze_sdk(rom, entry, bss, bss_size, main, extra_segments)
    # analysed where main maps it only when that is where the whole image runs: its functions there cover
    # most of what IPL3 copied (Conker: 0x24038 of 0x2C4B0 bytes). Perfect Dark's main maps 0x70000000
    # the same way, but past its first pages what runs there is code it inflates (0x7FAC of 0x89E20
    # covered): as one ROM segment there its learnt functions were compiled from the compressed bytes
    # (4 libultra names against 85, a crash), so it stays at KSEG0 with its captures, as before
    a_t = _analyze_tlb(rom, entry, (tm[0], tm[1], main), extra_segments,
                       load=entry & 0x1FFFFFFF, size=bss - entry, bss=bss)
    cover = sum(f.end - f.start for f in a_t.funcs.get('boot', {}).values())
    if 2 * cover >= bss - entry:
        a_t.kseg0_main = main   # the game's own, whatever libultra function it looks like (romonly)
        return a_t
    a = _analyze_sdk(rom, entry, bss, bss_size, main, extra_segments)
    a.notes.append(f'main {main:08X} maps {tm[1]:08X} onto physical 0 and jumps to {tm[0]:08X}, but code there '
                   f'covers only {cover:#x} of {bss - entry:#x} bytes: analysed at KSEG0')
    return a


def _analyze_sdk(rom, entry, bss, bss_size, main, extra_segments):
    """the SDK's shape: ROM 0x1000 at the entry, up to the BSS"""
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


def _analyze_tlb(rom, entry, t, extra_segments, load=0x400, size=None, bss=None):
    """the TLB template (funcs.tlb_entry): the entry and its TLB writer run at
    KSEG0 (a small segment of their own), the rest of what IPL3 copied runs
    where the TLB maps it: physical `load` (0x400; Conker's 6105 IPL3: 0x1000)
    at base + load (the game's own entry or main maps the pages; the runtime
    aliases them, rt/tlb.c)"""
    main, base, helper = t
    img = F.Image(rom)
    kseg0 = F.Segment('entry', 0x1000, entry, max(helper - entry + 0x100, 0x100))
    code = F.Segment('boot', 0x1000, base + load, size or min(0x100000, len(rom) - 0x1000))
    img.segments += [kseg0, code]
    a = Analysis(entry, main, bss if bss is not None else base + load + code.size, img)
    a.funcs['entry'] = F.discover(img, kseg0, [entry, helper])
    a.funcs['boot'] = F.discover(img, code, [main])
    a.notes.append(f'TLB-mapped code: the entry at {entry:08X} maps {base:08X} onto physical 0 and calls {main:08X}; '
                   f'ROM 0x1000 at {base + load:08X}, {code.size:#x} bytes, {len(a.funcs["boot"])} functions')
    for seg in extra_segments:
        _add(a, seg, [seg.vram])
    while True:
        new = S.find(img, list(a.funcs.values()), len(rom))
        if not new:
            break
        for seg, inside, pc in new:
            _add(a, seg, inside, S.weak(seg, inside))
            a.notes.append(f'{seg.name}: ROM {seg.rom:#x} at {seg.vram:08X}, {seg.size:#x} bytes')
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
