"""Yaz0-compressed ROMs (PLAN.md 3.5 "Compressed games", M5).

The Zelda engine (Ocarina of Time, Majora's Mask) keeps its files in the ROM
behind a DMA table of 16-byte entries: (vrom start, vrom end, prom start,
prom end). prom end 0 means the file is stored as is at prom start; otherwise
it is Yaz0-compressed there and the game's DMA manager inflates it on load.
The recompiler needs the code as it runs, so n64rx rebuilds the image the way
the community's decompressors do: every file at its vrom address,
uncompressed, and the table rewritten to (vrom, vrom end, vrom, 0), which the
game's own DMA manager reads as "not compressed". The console then checks the
SHA-1 of that image (it is what `$ROM` holds).

The table is found by what it is, not where one game keeps it: a run of
entries whose vrom ranges ascend without overlap, whose compressed entries
point at "Yaz0" headers that inflate to exactly their vrom size, and one of
which describes the table itself. Nothing here holds a game's addresses.
"""
import re
import struct

MAGIC = b'Yaz0'
ENTRY = 16


MAX_SIZE = 64 << 20   # no N64 file inflates past a cartridge's 64 MB


def decompress(src, offset=0, want=None, end=None):
    """the inflated bytes of the Yaz0 block at `offset`. want: the size its
    table entry says (checked before anything is allocated); end: where the
    block stops in `src`. A corrupt block raises ValueError (review 3.r2: a
    header claiming 256 MB allocated it, a back-reference before the
    output's start read the far end of the buffer silently)"""
    if src[offset:offset + 4] != MAGIC:
        raise ValueError('not a Yaz0 block')
    size = struct.unpack_from('>I', src, offset + 4)[0]
    if size > MAX_SIZE or (want is not None and size != want):
        raise ValueError(f'a Yaz0 header says {size} bytes' + (f', its table entry {want}' if want is not None else ''))
    end = len(src) if end is None else min(end, len(src))
    out = bytearray(size)
    s, d = offset + 16, 0
    while d < size:
        if s >= end:
            raise ValueError('a Yaz0 block ends before its data does')
        code = src[s]
        s += 1
        for bit in range(7, -1, -1):
            if d >= size:
                break
            if s >= end:
                raise ValueError('a Yaz0 block ends before its data does')
            if code & (1 << bit):
                out[d] = src[s]
                d += 1
                s += 1
            else:
                if s + 1 >= end:
                    raise ValueError('a Yaz0 block ends inside a back-reference')
                b1, b2 = src[s], src[s + 1]
                s += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    if s >= end:
                        raise ValueError('a Yaz0 block ends inside a back-reference')
                    n = src[s] + 0x12
                    s += 1
                else:
                    n += 2
                if dist > d:
                    raise ValueError(f'a Yaz0 back-reference {dist} bytes back at output byte {d}')
                for _ in range(n):
                    if d >= size:
                        break
                    out[d] = out[d - dist]
                    d += 1
    return bytes(out)


def _entry(rom, at):
    return struct.unpack_from('>IIII', rom, at)


def _plausible(rom, at):
    """the table's entries from `at`: [(vs, ve, ps, pe)] while they hold together"""
    out = []
    last_end = 0
    n = len(rom)
    while at + ENTRY <= n:
        vs, ve, ps, pe = _entry(rom, at)
        if vs == ve == ps == pe == 0:
            break
        if ve < vs or vs < last_end or ve - vs > 0x1000000:
            break
        if ps == pe == 0xFFFFFFFF:
            pass   # a file this ROM leaves out (Majora's Mask)
        elif pe:
            if pe <= ps or pe > n or rom[ps:ps + 4] != MAGIC:
                break
        elif ps != 0xFFFFFFFF and (ps + (ve - vs) > n):
            break
        out.append((vs, ve, ps, pe))
        last_end = ve
        at += ENTRY
    return out


def find_table(rom):
    """(offset, entries) of the DMA table, or None. The first entry of the
    Zelda engine's table is the boot area itself (vrom 0, prom 0, stored
    as is, some KB long); one entry is the table, so a candidate must
    contain its own offset."""
    if MAGIC not in rom:
        return None
    best = None
    # (0, end, 0, 0) with 0x1000 <= end < 0x200000, 16-byte aligned
    for m in _FIRST.finditer(rom):
        at = m.start()
        if at % ENTRY:
            continue
        ve = struct.unpack_from('>I', rom, at + 4)[0]
        if not 0x1000 <= ve <= 0x200000:
            continue
        ents = _plausible(rom, at)
        packed = sum(1 for e in ents if e[3] and e[2] != 0xFFFFFFFF)
        self_ref = any(e[0] <= at < e[1] and not e[3] for e in ents)
        if len(ents) >= 16 and packed >= 4 and self_ref and (best is None or len(ents) > len(best[1])):
            best = (at, ents)
    return best


_FIRST = re.compile(rb'(?=\x00{5}[\x00-\x1f][\x00-\xff]{2}\x00{8})(?!\x00{8})', re.S)   # zero width: every place, not just after the last match


def inflate_rom(rom):
    """(image, table offset, compressed files) for a Yaz0 ROM, or None when
    the ROM has no such table (left as it is)"""
    found = find_table(rom)
    if not found:
        return None
    at, ents = found
    size = max(e[1] for e in ents)
    if size > MAX_SIZE:
        raise ValueError(f'the DMA table reaches vrom {size:#x}, past 64 MB')
    size = (size + 0xFFFFF) & ~0xFFFFF   # whole MB, as cartridges are
    img = bytearray(size)
    packed = 0
    for vs, ve, ps, pe in ents:
        if ps == 0xFFFFFFFF:   # a file not in this ROM
            continue
        if pe:   # (absent files were skipped above)
            data = decompress(rom, ps, want=ve - vs, end=pe)
            if len(data) != ve - vs:
                raise ValueError(f'a Yaz0 file inflates to {len(data)} bytes, its table entry says {ve - vs}')
            packed += 1
        else:
            data = rom[ps:ps + (ve - vs)]
        img[vs:vs + len(data)] = data
    # the table as the image now holds it: every file at its vrom, uncompressed
    for i, (vs, ve, ps, pe) in enumerate(ents):
        if ps == 0xFFFFFFFF:
            continue
        struct.pack_into('>IIII', img, at + i * ENTRY, vs, ve, vs, 0)
    # the header's CRC covers 1 MB from 0x1000, unchanged by this (the boot area is never compressed)
    return bytes(img), at, packed
