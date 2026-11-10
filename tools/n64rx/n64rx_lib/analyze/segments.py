"""Segments loaded with constant addresses (PLAN.md 3.14, step 4).

A game copies its other code from the cartridge with a DMA whose ROM
address, RAM address and size are link-time constants (`lui`/`addiu`
pairs, `end - start` with a `subu`), passed to osPiStartDma or to the
game's own wrapper around it. So every call in the known code is looked at
with its arguments evaluated by a straight-line constant propagation
(registers and the outgoing stack slots 0x10-0x1C): a call whose arguments
hold a ROM offset R, a KSEG0 address V and a size N, with R + N inside the
ROM and V outside every known segment, is a candidate. A candidate becomes a
segment when the known code calls into it (a `jal` whose target falls in
[V, V + N)): its bytes at V are then the code being called. Nothing here
needs the callee to be identified.

Overlays: V may lie inside a known segment when the ROM range is another
one (levels loaded one at a time at the same address). A call counts as
"into the unknown" when no known segment has a function at its target, so
a call into an overlay's address range that the overlay found first does
not explain brings in the next one. Kill switch: N64RX_OVERLAYS=0 (the
old rule: V outside every known segment)."""
import os
from .funcs import Segment
from .mips import Ins, SP

ARG_REGS = (4, 5, 6, 7)


def _consts(img, f):
    """(jal pc, target, {arg: value}) for every call in f, args by constant
    propagation along the function's straight-line order (kept per
    function: the bytes do not change between the segment search's rounds)"""
    cache = img.__dict__.setdefault('_consts_cache', {})
    s = img.seg(f.start)
    key = (f.start, f.end, s.rom - s.vram if s else None)
    if key not in cache:
        cache[key] = _consts_walk(img, f)
    return cache[key]


def _consts_walk(img, f):
    regs, stack, out = {0: 0}, {}, []
    pc = f.start
    while pc < f.end:
        w = img.word(pc)
        if w is None:
            break
        ins = Ins(w, pc)
        if ins.is_jal or ins.is_jalr:
            ds = img.word(pc + 4)
            if ds is not None:
                _step(Ins(ds, pc + 4), regs, stack)   # the delay slot runs before the callee
            if ins.is_jal:
                args = {r: regs[r] for r in ARG_REGS if r in regs}
                for off in (0x10, 0x14, 0x18, 0x1C):
                    if off in stack:
                        args[off] = stack[off]
                out.append((pc, ins.target, args))
            for r in (1, 2, 3, *ARG_REGS, *range(8, 16), 24, 25, 31):   # caller-saved die
                regs.pop(r, None)
            pc += 8
            continue
        if ins.is_branch or ins.is_j or ins.is_jr:   # a join: keep it simple, forget nothing but what is written
            pass
        _step(ins, regs, stack)
        pc += 4
    return out


def _step(ins, regs, stack):
    op, d = ins.op, ins.dest
    v = None
    if op == 15:                                       # lui
        v = ins.imm << 16
    elif op in (8, 9) and ins.rs in regs:              # addi / addiu
        v = regs[ins.rs] + ins.simm
    elif op == 13 and ins.rs in regs:                  # ori
        v = regs[ins.rs] | ins.imm
    elif op == 0 and ins.funct in (32, 33) and ins.rs in regs and ins.rt in regs:   # add / addu
        v = regs[ins.rs] + regs[ins.rt]
    elif op == 0 and ins.funct in (34, 35) and ins.rs in regs and ins.rt in regs:   # sub / subu
        v = regs[ins.rs] - regs[ins.rt]
    elif op == 0 and ins.funct in (36, 37, 38) and ins.rs in regs and ins.rt in regs:   # and / or / xor (move)
        a, b = regs[ins.rs], regs[ins.rt]
        v = a & b if ins.funct == 36 else a | b if ins.funct == 37 else a ^ b
    elif op == 12 and ins.rs in regs:                  # andi
        v = regs[ins.rs] & ins.imm
    elif op == 0 and ins.funct in (0, 2) and ins.rt in regs and ins.w:   # sll / srl
        v = regs[ins.rt] << ins.sa if ins.funct == 0 else (regs[ins.rt] & 0xFFFFFFFF) >> ins.sa
    elif op == 35 and ins.rs == SP and ins.simm in stack:   # lw reg, off(sp): a local kept on the stack (-O0)
        v = stack[ins.simm]
    elif op == 43 and ins.rs == SP:                    # sw reg, off(sp): an outgoing argument or a local
        if ins.rt in regs:
            stack[ins.simm] = regs[ins.rt]
        else:
            stack.pop(ins.simm, None)
        return
    if d is not None:
        if v is None:
            regs.pop(d, None)
        else:
            regs[d] = v & 0xFFFFFFFF


ORDER = (4, 5, 6, 7, 0x10, 0x14, 0x18, 0x1C)


def candidates(img, funcs, rom_size):
    """(rom, vram, size, call pc, natural) for every call that looks like a
    DMA of code. `natural` 2: a ROM start and end next to each other with
    the RAM address before or after them ((dest, start, end), (start, end,
    dest)); 1: the size is the argument right after the ROM and
    RAM addresses, where every DMA call puts it ((dst, src, size),
    osPiStartDma's (devAddr, dramAddr, size))."""
    out = set()
    for f in funcs.values():
        for pc, _tgt, args in _consts(img, f):
            pos = {k: i for i, k in enumerate(ORDER)}
            for kr, r in args.items():
                if not 0x1000 <= r < rom_size:
                    continue
                for kv, v in args.items():
                    if not 0x80000000 <= v < 0x80800000 or _taken(img, r, v):
                        continue
                    after = ORDER[max(pos[kr], pos[kv]) + 1] if max(pos[kr], pos[kv]) + 1 < len(ORDER) else None
                    for kn, n in args.items():
                        if kn in (kr, kv) or not 0x100 <= n <= 0x400000 or r + n > rom_size:
                            continue
                        out.add((r, v, n, pc, 1 if kn == after else 0))
                    # (ROM start, ROM end, RAM): the size is the difference (a
                    # game's dma_copy(start, end, dest) shape)
                    for ke, e in args.items():
                        if ke in (kr, kv) or not 0x100 <= e - r <= 0x400000 or e > rom_size:
                            continue
                        shape = (pos[ke] == pos[kr] + 1 and pos[kv] in (pos[ke] + 1, pos[kr] - 1))   # (start, end, dest), (dest, start, end)
                        out.add((r, v, e - r, pc, 2 if shape else 0))
    return sorted(out)


def in_data(img, rom_size):
    """(rom, vram, size, where, 2) for every (RAM, ROM start, ROM end) or
    (ROM start, ROM end, RAM) word triple anywhere in the ROM: the loads a
    game describes in data rather than in code (a script's load command, an
    overlay table). The same test as for a call decides which are code."""
    out = set()
    for i, v, r, e, text, text_end in _triples(img.rom, rom_size):
        if v >= 0xC0000000 or _taken(img, r, v):   # KSEG2/3: only for a function learnt there (learn.apply)
            continue
        out.add((r, v, e - r, f'a load described in ROM data at {4 * i:#x}', 3 if text else 2))
        if text:
            img.text_ends[(r, v)] = text_end
    return out


_TRIPLES = {}


def _triples(rom, rom_size):
    """the word triples in_data tests, found once per ROM (the scan of every
    word was most of Mario Party's analysis, repeated each round)"""
    key = (id(rom), len(rom), rom_size)
    if key in _TRIPLES:
        return _TRIPLES[key]
    import struct
    ws = struct.unpack(f'>{len(rom) // 4}I', rom[:len(rom) // 4 * 4])
    out = []
    for i in range(len(ws) - 2):
        a, b, c = ws[i], ws[i + 1], ws[i + 2]
        for v, r, e in ((a, b, c), (c, a, b)):
            if (0x80000400 <= v < 0x80800000 or (HIGH_SEGS and 0xC0000000 <= v < 0xF0000000)) and not v & 3 and \
                    0x1000 <= r < e <= rom_size and not r & 3 and 0x100 <= e - r <= 0x400000:
                # (start, end, RAM, text start, text end, ...): a descriptor
                # that says where its code is (HAL's overlay tables) names a
                # segment even when no known code calls into it (a scene's
                # entry reached through a pointer): natural 3
                text = (OVERLAYS and (v, r, e) == (c, a, b) and i + 4 < len(ws) and ws[i + 3] == v and
                        v < ws[i + 4] <= v + (e - r) and not ws[i + 4] & 3)
                out.append((i, v, r, e, text, ws[i + 4] if text else 0))
    _TRIPLES.clear()
    _TRIPLES[key] = out
    return out


OVERLAYS = os.environ.get('N64RX_OVERLAYS', '1') != '0'
# Body Harvest loads three code blocks at 0x80070270 from three constant DMAs
# (800115F4: ROM 0x40720, 0x3EB00 bytes, the title; 80011674: ROM 0x7F220,
# 0xD9110; 800117D8: ROM 0x158330, 0x354B0) and calls 0x80070270 after each.
# find() kept one segment per address (the smallest, 0x158330), so the
# runtime ran that block's code where the title's was loaded: its 8007290C
# read the bank buffer *0x8006AA6C before any bank was loaded, DMA'd an MIO0
# block to address 0 (over the exception vectors), and the next call into
# 0x80000B14 ran the MIO0 header. Loads at the same address from ROM ranges
# that do not overlap, their size in its natural argument, are kept
# together, overlays of one another (the runtime switches on the DMA;
# Analysis.overlays keys them apart). N64RX_OVERLAY_SIBLINGS=0: one per
# address, as before
SIBLINGS = os.environ.get('N64RX_OVERLAY_SIBLINGS', '1') != '0'
# The DMA's own destination, called, is a function start whatever its first
# word (Body Harvest's title block opens with `addiu t6, a0, -1`, its frame
# one word later). N64RX_LOAD_START=0: as before
LOAD_START = os.environ.get('N64RX_LOAD_START', '1') != '0'
# loads described in ROM data to KSEG2/3 addresses (0xC0000000-0xEFFFFFFF): code the game maps
# through its TLB where it loaded it (Paper Mario's 128 effects, each its own vram at 0xE0xxxxxx; the
# runtime reaches their data through the game's TLB, XRC_HIGH_TLB). Most such triples are other words
# (lwc1 and the like), so they are used only for a function the console ran there (learn.apply,
# high_load). N64RX_HIGH_SEGS=0: KSEG0 only
HIGH_SEGS = os.environ.get('N64RX_HIGH_SEGS', '1') != '0'
# A described text (a load table's start, end and RAM) whose only bad walks are a few `jr` with no table: those functions
# are interpreted (romonly.py), the rest of the overlay is recompiled, instead of losing it whole. WWF No Mercy Plus V3's
# two menu overlays at 0x80107C60 (ROM 0xAE400: 1 of 660 functions, 0xFD2C0: 5 of 344) were left out, and the one overlay
# found there ran on their data (a crash after START). N64RX_OVL_JR=1: for every ROM; 0: never; unset: only a ROM with an
# entry hook (a ROM hack, funcs.hook_copies) or one whose games/patches file says `n64rx: ovl_jr` (WWF WrestleMania 2000),
# so other retail builds stay as they were
OVL_JR = os.environ.get('N64RX_OVL_JR', '')
OVL_JR_SHARE = 0.05   # at most this share of the overlay's functions
_OVL_JR_ROM = {}      # id of a ROM image -> its games/patches file asks for it


def _rom_asks_ovl_jr(img):
    """the ROM's own file in games/patches (<code>.<sha1[:12]>.c) says `n64rx: ovl_jr`: WWF WrestleMania 2000 (NWXE) loses its
    match overlay (ROM 0xD2720 at 0x800E1B90, a few `jr` without a table) and crashed entering a match without it"""
    key = id(img.rom)
    if key not in _OVL_JR_ROM:
        import hashlib
        import pathlib
        rom = bytes(img.rom)
        code = rom[0x3B:0x3F].decode('ascii', 'replace')
        f = pathlib.Path(__file__).resolve().parents[4] / 'games' / 'patches' / f'{code}.{hashlib.sha1(rom).hexdigest()[:12]}.c'
        _OVL_JR_ROM[key] = f.is_file() and 'n64rx: ovl_jr' in f.read_text(encoding='utf-8')
    return _OVL_JR_ROM[key]


def _jr_only(img, fs):
    """the walks of a described text are clean but for a few `jr` without a table, under N64RX_OVL_JR"""
    if OVL_JR == '0' or (OVL_JR != '1' and not any(s.name.startswith('hook_') for s in img.segments)
                         and not _rom_asks_ovl_jr(img)):
        return False
    bad = [f for f in fs.values() if f.bad]
    return bool(bad) and all(f.bad.startswith('jr $') for f in bad) and len(bad) <= OVL_JR_SHARE * len(fs)


def _taken(img, r, v):
    """a load to v from ROM r explains nothing new: v is in a known segment
    (with overlays off), in the boot segment, or r is a known segment's ROM"""
    if not OVERLAYS:
        return img.seg(v) is not None
    for i, s in enumerate(img.segments):
        if s.rom <= r < s.rom + s.size or (i == 0 and s.has(v)):
            return True
    return False


def _starts_like_function(img, t):
    """the word at t opens a frame (addiu sp, sp, -n), or the words before it
    close one (jr ra and its delay slot) or pad (zero)"""
    w = img.word(t)
    if w is None:
        return False
    i = Ins(w, t)
    if i.op == 9 and i.rs == SP and i.rt == SP and i.simm < 0:
        return True
    p1, p2 = img.word(t - 4), img.word(t - 8)
    return p1 == 0 or p2 == 0x03E00008


def weak(seg, inside):
    """the roots a call gave that may be another overlay's (every one but
    the start of a described text), with overlays on"""
    return [t for t in inside if t != seg.vram or not seg.code_end] if OVERLAYS else []


def _name(img, r, v):
    """seg_<vram>, and seg_<vram>_<rom> when a known segment has that name
    (two overlays at one address)"""
    n = f'seg_{v:08X}'
    return n if all(s.name != n for s in img.segments) else f'{n}_{r:X}'


def find(img, funcs_by_seg, rom_size):
    """New segments: candidates the known code calls into. Each (ROM, vram,
    size) a call offers is tried: the bytes at every call target must look
    like a function start, and every walk from them must be clean; the best
    score wins: a size in its natural argument first, then the share of
    targets that look like starts, then the most targets, then the
    smallest size."""
    from .funcs import discover
    known = [f for fs in funcs_by_seg for f in fs.values()]
    starts = {f.start for f in known}
    targets = set()
    for f in known:
        targets |= {t for t in f.calls | f.tails if (t not in starts if OVERLAYS else not img.seg(t))}
    allf = {f.start: f for f in known}
    trials = []
    found_in = list(candidates(img, allf, rom_size))
    if targets or OVERLAYS:
        found_in += sorted(in_data(img, rom_size), key=lambda c: c[:3])
    for r, v, n, pc, natural in found_in:
        inside = sorted(t for t in targets if v <= t < v + n)
        if not inside and natural != 3:
            continue
        seg = Segment(_name(img, r, v), r, v, n, img.text_ends.get((r, v), 0) if natural == 3 else 0)
        img.segments.append(seg)
        prev, img.focus = img.focus, seg
        like = [t for t in inside if (LOAD_START and t == v) or _starts_like_function(img, t)]
        if natural == 3:
            # the text starts with a function (the walks from it must be
            # clean); targets that do not look like starts are another
            # overlay's at the same address
            like = inside = sorted(set(like) | {v})
        img.focus = prev
        score = len(like) / len(inside)
        ok = score >= 0.9
        if not ok and OVERLAYS and natural >= 2 and score >= 0.75 and len(like) >= 4:
            # a load table's (start, end, RAM): where overlays share the
            # range some targets are another overlay's; the ones that look
            # like starts are this one's
            ok, inside = True, like
        if ok:
            fs = discover(img, seg, inside, weak(seg, inside))
            ok = bool(fs) and (not any(f.bad for f in fs.values()) or (natural == 3 and _jr_only(img, fs)))
        img.segments.remove(seg)
        if ok:
            trials.append((-natural, -score, -len(inside), n, seg, inside, pc))
    found = []
    for _a, _s, _l, _n, seg, inside, pc in sorted(trials, key=lambda t: t[:4]):
        if any((o.has(seg.vram) or seg.has(o.vram)) and
               not (SIBLINGS and -_a >= 1 and o.vram == seg.vram and
                    not (o.rom < seg.rom + seg.size and seg.rom < o.rom + o.size))
               for o, _i, _p in found):
            continue
        if any(o.name == seg.name for o, _i, _p in found):   # siblings: named apart (keys: Analysis.key)
            seg.name = f'{seg.name}_{seg.rom:X}'
        found.append((seg, inside, pc))
    return found


def high_load(img, rom_size, x):
    """the load a ROM table describes to a KSEG2/3 vram range holding x (a
    function learnt there), as (rom, vram, size), when exactly one does: a
    page-aligned vram (the TLB maps whole pages), at most 1 MB, and the
    word at x in its bytes the start of a function (Paper Mario's effect
    table names each effect's code and its graphics data at one vram)"""
    import struct

    def word(o):
        return struct.unpack('>I', img.rom[o:o + 4])[0] if 0 <= o and o + 4 <= len(img.rom) else None

    hits = set()
    for _i, v, r, e, _t, _te in _triples(img.rom, rom_size):
        if v < 0xC0000000 or v & 0xFFF or not v <= x < v + (e - r) or e - r > 0x100000:
            continue
        w, back = word(r + x - v), word(r + x - v - 8)
        if (w is not None and w >> 16 == 0x27BD and w & 0x8000) or back == 0x03E00008 or x == v:
            hits.add((r, v, e - r))
    return hits.pop() if len(hits) == 1 else None
