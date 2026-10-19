"""Overlays a game relocates itself (PLAN.md 3.4 "Overlays", 3.14).

The Zelda engine (Ocarina of Time, Majora's Mask) links each overlay (an
actor, a game state, the pause screens) at an address of its own above the
RAM (0x80800000 and up) and loads it wherever its allocator gives room; its
loader then patches the code and data in RDRAM from a relocation table the
file carries at its end (Overlay_Relocate in the decomps). Code no single
address holds was the safety net's: interpreted, a hitch at every scene
load. Here each such file becomes a relocatable section at its link
address, with its relocations, so N64Recomp emits RELOC_HI16/LO16 over
`section_addresses` for its own references; the runtime maps the section
where the game relocated it (rt/ovl.c xrc_ovl_relocate, called from a hook
at the start of the game's relocation routine).

Found by what they are, not where one game keeps them:
  - the DMA table of a Yaz0 game's image (yaz0.py inflates every file to
    its vrom, so the overlay is in the build's ROM image as it runs);
  - a file whose last word is the distance back to a header of five words
    (text, data, rodata, bss sizes, the number of relocations) that starts
    right after its text, data and rodata, followed by that many
    relocations (2-bit section, 6-bit type: R_MIPS_32/26/HI16/LO16, 24-bit
    offset inside the section);
  - its link address from the game's own overlay tables in the code: the
    file's (vrom start, vrom end) followed by (vram start, vram end) whose
    span is the whole file (its relocations are loaded with it) plus its
    bss.
Nothing here holds a game's addresses. N64RX_RELOC_OVL=0: not looked for
(the safety net runs the overlays, as before); =all: every overlay found is
recompiled, not only those a learning run saw relocated (romonly.py)."""
import os
import struct
from dataclasses import dataclass, field

ENABLE = os.environ.get('N64RX_RELOC_OVL', '1') != '0'
TYPES = {2: 'R_MIPS_32', 4: 'R_MIPS_26', 5: 'R_MIPS_HI16', 6: 'R_MIPS_LO16'}
SEC_TEXT, SEC_DATA, SEC_RODATA = 1, 2, 3


@dataclass
class Overlay:
    vrom: int
    vend: int
    text: int
    data: int
    rodata: int
    bss: int
    relocs: tuple
    vram: int = 0
    code_relocs: list = field(default_factory=list)   # [(vram, target_vram, type name)] in the text
    pointers: list = field(default_factory=list)      # text addresses its data points at (function pointers)

    @property
    def span(self):   # its vram range: the whole file (the relocation section is loaded too) and its bss
        return self.vend - self.vrom + self.bss


def _u32(b, at):
    return struct.unpack_from('>I', b, at)[0]


def dma_table(rom):
    """(offset, [(vs, ve, ps, pe)]) of a Zelda-engine DMA table in `rom` (compressed or
    inflated), or None"""
    from .. import yaz0
    best = None
    for m in yaz0._FIRST.finditer(rom):
        at = m.start()
        if at % yaz0.ENTRY:
            continue
        ve = _u32(rom, at + 4)
        if not 0x1000 <= ve <= 0x200000:
            continue
        ents = yaz0._plausible(rom, at)
        if len(ents) >= 16 and any(e[0] <= at < e[1] for e in ents) and (best is None or len(ents) > len(best[1])):
            best = (at, ents)
    return best


def overlay_of(rom, vs, ve):
    """the overlay layout of the file [vs, ve) of an inflated image, or None"""
    n = ve - vs
    if n < 0x40 or ve > len(rom):
        return None
    back = _u32(rom, ve - 4)
    if back % 4 or not 0x14 <= back <= n - 4:
        return None
    hdr = ve - back
    text, data, rodata, bss, nrel = struct.unpack_from('>5I', rom, hdr)
    if not text or text % 4 or text + data + rodata != hdr - vs or 20 + 4 * nrel > back or bss > 0x100000:
        return None
    relocs = struct.unpack_from(f'>{nrel}I', rom, hdr + 20)
    sizes = {SEC_TEXT: text, SEC_DATA: data, SEC_RODATA: rodata}
    for r in relocs:
        if r >> 30 not in sizes or (r >> 24) & 0x3F not in TYPES or (r & 0xFFFFFF) >= sizes[r >> 30]:
            return None
    return Overlay(vs, ve, text, data, rodata, bss, relocs)


def _link_addresses(rom, ovls, ranges):
    """each overlay's vram from the (vrom start, vrom end, vram start, vram end) runs in `ranges`
    (the code's ROM ranges)"""
    by = {(o.vrom, o.vend): o for o in ovls}
    for lo, hi in ranges:
        for at in range(lo & ~3, min(hi, len(rom)) - 16, 4):
            o = by.get(struct.unpack_from('>II', rom, at))
            if o is None or o.vram:
                continue
            v0, v1 = struct.unpack_from('>II', rom, at + 8)
            if 0x80000000 <= v0 < v1 < 0x81000000 and v1 - v0 == o.span and not v0 & 0xF:
                o.vram = v0


def _relocate_view(rom, o):
    """the relocations as N64Recomp takes them: text instructions only, each with the link address it
    names inside the overlay (a HI16 paired with the LO16 that reads its register, as the game's own
    Overlay_Relocate pairs them); and the text addresses the data and rodata point at"""
    base = {SEC_TEXT: o.vram, SEC_DATA: o.vram + o.text, SEC_RODATA: o.vram + o.text + o.data}
    rbase = {SEC_TEXT: o.vrom, SEC_DATA: o.vrom + o.text, SEC_RODATA: o.vrom + o.text + o.data}
    lo_end = o.vram + o.span
    hi_at, hi_word = {}, {}
    hi_done = set()
    for r in o.relocs:
        sec, typ, off = r >> 30, (r >> 24) & 0x3F, r & 0xFFFFFF
        w = _u32(rom, rbase[sec] + off)
        at = base[sec] + off
        if typ == 2:   # a pointer in data: a function pointer when it points into the text
            if sec != SEC_TEXT and o.vram <= w < o.vram + o.text and sec == SEC_DATA:
                o.pointers.append(w)
            continue
        if sec != SEC_TEXT:
            continue
        if typ == 4:
            t = 0x80000000 | ((w & 0x03FFFFFF) << 2)
            if o.vram <= t < lo_end:
                o.code_relocs.append((at, t, 'R_MIPS_26'))
        elif typ == 5:
            rt = (w >> 16) & 31
            hi_at[rt], hi_word[rt] = at, w
        elif typ == 6:
            rs = (w >> 21) & 31
            if rs not in hi_at:
                continue
            lo = w & 0xFFFF
            t = (((hi_word[rs] & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            if not o.vram <= t < lo_end:
                continue
            if hi_at[rs] not in hi_done:
                o.code_relocs.append((hi_at[rs], t, 'R_MIPS_HI16'))
                hi_done.add(hi_at[rs])
            o.code_relocs.append((at, t, 'R_MIPS_LO16'))
    o.code_relocs.sort()   # N64Recomp walks a section's relocations in address order with its instructions


def find(rom, code_ranges):
    """the relocatable overlays of an inflated Zelda-engine image, each with its link address"""
    if not ENABLE:
        return []
    found = dma_table(rom)
    if not found:
        return []
    _at, ents = found
    ovls = []
    for vs, ve, ps, _pe in ents:
        if ps == 0xFFFFFFFF:
            continue
        o = overlay_of(rom, vs, ve)
        if o:
            ovls.append(o)
    if not ovls:
        return []
    _link_addresses(rom, ovls, code_ranges)
    out = []
    for o in ovls:
        if not o.vram:
            continue
        _relocate_view(rom, o)
        out.append(o)
    # two files with one link address (a copy of an overlay): neither, rather than the wrong one
    seen = {}
    for o in out:
        seen.setdefault(o.vram, []).append(o)
    return [o for o in out if len(seen[o.vram]) == 1]
