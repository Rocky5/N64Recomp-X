"""Function discovery from the ROM (PLAN.md 3.14, step 1-2).

Image: the ROM (z64 order) and the segments known so far, each a ROM range
at a vram. The first is the boot segment: IPL3 copies 1 MB from ROM 0x1000
to the entrypoint, and the SDK's entry code clears the BSS that follows the
segment's loaded part, so the segment ends where the BSS starts
(`parse_entry`).

discover(): recursive descent from the roots (the entry, its jump to main)
over every jal; each function is walked through its branches to its last
reachable instruction; then the gaps between functions inside the code are
tried as functions, and words in the segment's data that point at the
start of such a gap. The result is what N64Recomp needs: contiguous,
non-overlapping functions, each sized to its last instruction."""
import os
from dataclasses import dataclass, field

from .mips import Ins, RA

MAX_FUNC = 0x10000          # a function larger than this is a walk gone wrong
INTRA_J = 0x2000            # a j this close and not onto a known start stays in the function
WALK_LATER = os.environ.get('N64RX_J_LATER', '1') != '0'   # a far j decided after the walk (kill switch)

KUSEG_TABLES = os.environ.get('N64RX_KUSEG_TABLES', '1') != '0'
GAP_DATA = os.environ.get('N64RX_GAP_DATA', '1') != '0'
DATA_OPS = {0x30, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E}   # ll lwc2 lld ldc2 sc swc2 scd sdc2


@dataclass
class Segment:
    name: str
    rom: int
    vram: int
    size: int
    code_end: int = 0               # where its text ends, when a load descriptor says (0: unknown)

    def has(self, v):
        return self.vram <= v < self.vram + self.size


@dataclass
class Func:
    start: int
    end: int                            # exclusive, last instruction + 4
    calls: set = field(default_factory=set)
    tails: set = field(default_factory=set)     # j out of the function: tail calls
    jtables: list = field(default_factory=list)  # (jr pc, table vram, entries)
    how: str = 'call'                   # call | entry | gap | pointer
    bad: str = ''                       # why the walk is suspect ('' = clean)
    table_want: int = 0                 # a jr's table address whose bytes no segment holds (captured code)


class Image:
    """`focus`: the segment being walked; where overlays share an address it
    is the one read (an overlay's code reads its own bytes)"""
    def __init__(self, rom):
        self.rom = rom
        self.segments = []
        self.focus = None
        self.table_sources = []     # learn.py: captured RDRAM read for what lies outside the walked segment
        self.text_ends = {}         # (rom, vram) -> text end, from load descriptors (segments.in_data)

    def seg(self, v):
        if self.focus is not None and self.focus.has(v):
            return self.focus
        # the segments that touch v's 4 KB page, in list order (the first that holds v wins, as a scan
        # of the list would): Mario Party's 131 segments made the scan most of a 28-minute analysis
        key = (len(self.segments), id(self.segments[-1]) if self.segments else 0,
               sum(s.size for s in self.segments[-4:]))
        if getattr(self, '_pages_key', None) != key:
            self._pages, self._pages_key = {}, key
        page = v >> 12
        cands = self._pages.get(page)
        if cands is None:
            lo, hi = page << 12, (page << 12) + 0x1000
            cands = [s for s in self.segments if s.vram < hi and s.vram + s.size > lo]
            self._pages[page] = cands
        for s in cands:
            if s.has(v):
                return s
        return None

    def word(self, v):
        s = None
        if self.table_sources and not (self.focus is not None and self.focus.has(v)):
            # captured RDRAM over a segment its boot decompressed (learn.py _table_sources): what a
            # function outside it reads there, its jump tables (GoldenEye's at 0x8005xxxx)
            s = next((t for t in self.table_sources if t.has(v)), None)
        s = s or self.seg(v)
        if not s:
            return None
        o = s.rom + v - s.vram
        if o + 4 > len(self.rom):
            return None
        return int.from_bytes(self.rom[o:o + 4], 'big')


def parse_entry(rom, entry):
    """The SDK entry: lui/addiu t0 = BSS start, lui/ori t1 = BSS size, a clear
    loop, then the stack and a jump to main (jr t2 or j main). The variants
    other toolchains wrote are read too: the loop from a start to an end
    address (t1 = the end, `sw zero, -4(t0)` after the increment) and main
    called with jal (Forsaken, Turok 2: the entry spins after main returns).
    Returns (bss_start, bss_size, main) or None when the entry is something
    else."""
    regs, bss, size, main, snap = {}, None, None, None, None
    empty = False
    last_call = None
    for i in range(64):
        o = 0x1000 + 4 * i
        ins = Ins(int.from_bytes(rom[o:o + 4], 'big'), entry + 4 * i)
        d = ins.dest
        if ins.is_branch and snap is None and bss is None:
            snap = dict(regs)   # the registers as the loop starts (before its increments)
        if ins.op == 15:   # lui
            regs[ins.rt] = ins.imm << 16
        elif ins.op in (9, 8) and ins.rs in regs:   # addiu / addi
            regs[ins.rt] = (regs[ins.rs] + ins.simm) & 0xFFFFFFFF
        elif ins.op == 13 and ins.rs in regs:      # ori
            regs[ins.rt] = regs[ins.rs] | ins.imm
        elif ins.op == 43 and ins.rt == 0 and bss is None and ins.rs in regs:   # sw zero, off(t0): the clear loop
            src = snap if snap is not None and ins.rs in snap else regs
            bss = src[ins.rs]
            other = src.get(9)
            if other is not None and other >= 0x80000000 and other > bss:   # an end address, not a size
                size = other - bss
            elif other is not None and other >= 0x80000000:   # start == end: a loop over nothing
                bss, size, snap = None, None, None                # (Extreme-G 2 clears two empty ranges)
                empty = True
                continue
            else:
                size = other
        elif ins.is_jr:
            main = regs.get(ins.rs)
            empty = empty or (bss is None and i < 8 and 29 in regs)   # the stack, then jr to a loader (Turok)
            break
        elif ins.w & 0xFC00003F == 0x0000000D and last_call is not None and bss is None:
            main, empty = last_call, True   # break after the last call: that call was main (Army Men)
            break
        elif ins.is_j:
            main = ins.target
            empty = empty or (bss is None and i < 8 and 29 in regs)   # the stack, then j (Rogue Squadron)
            break
        elif ins.is_jalr:   # a call through a register: main only if it lands in KSEG0
            t = regs.get(ins.rs)   # (Turok 2, Armorines: jalr to TLB-mapped code, not supported)
            last_call = t if t is not None and t >= 0x80000000 else None
        elif ins.is_jal and not (bss is not None or empty or (i < 8 and 29 in regs)):
            last_call = ins.target if ins.target >= 0x80000000 else None   # a set-up call, or main before a break
        elif ins.is_jal and (bss is not None or empty or (i < 8 and 29 in regs)):
            main = ins.target   # main called, the entry spins after it; or the stack set
            empty = empty or bss is None   # and main called at once, no BSS clear (Madden 2000, Bust-A-Move '99)
            break
        elif d is not None and d not in (8, 9, 10, 29):
            regs.pop(d, None)
    if main is None or (bss is None and not empty):
        return None
    if bss is None:   # no BSS cleared: the boot segment is what IPL3 copied, 1 MB
        return entry + 0x100000, 0, main
    return bss, size or 0, main


def tlb_entry(rom, entry):
    """An entry that maps its own code with the TLB and calls into it (Acclaim's
    template: Turok 2 and 3, Rage Wars, Armorines): a call to a TLB writer with
    the virtual address in a2 and the even page's physical address in a3, then
    a jalr to a constant below 0x80000000, then a break. Returns (main, base,
    helper) with base = the virtual address of physical 0 (the code IPL3 copied
    to physical 0x400 runs at base + 0x400), or None."""
    regs, helper, va, pa, odd = {}, None, None, None, None
    for i in range(64):
        o = 0x1000 + 4 * i
        ins = Ins(int.from_bytes(rom[o:o + 4], 'big'), entry + 4 * i)
        if ins.op == 15:
            regs[ins.rt] = ins.imm << 16
        elif ins.op in (9, 8) and (ins.rs in regs or ins.rs == 0):
            regs[ins.rt] = ((regs.get(ins.rs, 0)) + ins.simm) & 0xFFFFFFFF
        elif ins.op == 13 and (ins.rs in regs or ins.rs == 0):
            regs[ins.rt] = regs.get(ins.rs, 0) | ins.imm
        elif ins.op == 43 and ins.rs == 29 and ins.simm == 0x10:   # sw rt, 0x10(sp): osMapTLB's oddpaddr
            odd = 0 if ins.rt == 0 else regs.get(ins.rt)
        elif ins.is_jal:
            if 6 in regs and 7 in regs and regs[6] < 0x80000000:
                helper, va, pa = ins.target, regs[6], regs[7]
                # osMapTLB(index, pagemask, vaddr, evenpaddr, oddpaddr, asid) with no even page (-1):
                # the odd page maps the code, half the pair above vaddr (Shadowman: 1 MB pages, the
                # odd one at 0x100000 onto physical 0; read as -1 the base came out 1, no code found).
                # N64RX_TLB_ODD=0: the even argument whatever it is, as before
                if pa == 0xFFFFFFFF and odd is not None and 5 in regs and os.environ.get('N64RX_TLB_ODD', '1') != '0':
                    va, pa = (va + (((regs[5] | 0x1FFF) + 1) >> 1)) & 0xFFFFFFFF, odd
        elif ins.is_jalr:
            t = regs.get(ins.rs)
            if helper is None or t is None or t >= 0x80000000:
                return None
            base = (va - pa) & 0xFFFFFFFF
            if not base + 0x400 <= t < base + 0x200400:
                return None
            return t, base, helper
        elif ins.dest is not None:
            regs.pop(ins.dest, None)
    return None


def _table(img, insns, i, fstart):
    """A jump table at the jr at insns[i]: walk back for lw rX, lo(rB) and
    lui rB, hi (with an addu of the index between), bounded by an earlier
    sltiu. Returns (table vram, [targets]) or None."""
    jr = insns[i]
    reg, lo, base_reg, count = jr.rs, None, None, None
    for k in range(i - 1, max(-1, i - 16), -1):
        ins = insns[k]
        if lo is None and ins.op == 35 and ins.rt == reg:            # lw rX, lo(rB)
            lo, base_reg = ins.simm, ins.rs
        elif lo is not None and ins.op == 15 and ins.rt == base_reg:  # lui rB, hi
            table = ((ins.imm << 16) + lo) & 0xFFFFFFFF
            for kk in range(k, max(-1, i - 24), -1):                 # the bound check
                if insns[kk].op == 11:                                # sltiu
                    count = insns[kk].imm
                    break
            targets = []
            for n in range(count if count else 512):
                t = img.word(table + 4 * n)
                if t is None or t & 3 or not (fstart <= t < fstart + MAX_FUNC):
                    break
                targets.append(t)
            return (table, targets) if targets else None
    return None


def _table_linear(img, fstart, pc, addr_only=False):
    """A jump table whose base was set up away from the jr (GCC hoists the
    lui/addiu out of a loop and adds the scaled index into another register):
    the lw before the jr, the addu that formed its address, and the lui/addiu
    of the base, each found as the last write in the code before it."""
    def last_def(reg, before, limit):
        v = before - 4
        while v >= max(fstart, before - 4 * limit):
            w = img.word(v)
            if w is None:
                return None
            i = Ins(w, v)
            if i.dest == reg:
                return i
            v -= 4
        return None

    def const(reg, before, depth=0):
        d = last_def(reg, before, 256)
        if d is None or depth > 4:
            return None
        if d.op == 15:
            return d.imm << 16
        if d.op in (8, 9, 13):
            b = const(d.rs, d.pc, depth + 1)
            return None if b is None else ((b | d.imm) if d.op == 13 else (b + d.simm)) & 0xFFFFFFFF
        return None
    jr = Ins(img.word(pc), pc)
    lw = last_def(jr.rs, pc, 16)
    if lw is None or lw.op != 35:
        return None
    d = last_def(lw.rs, lw.pc, 32)
    if d is None or not (d.op == 0 and d.funct in (32, 33)):
        return None
    table = None
    for x in (d.rs, d.rt):
        b = const(x, d.pc)
        # (a KUSEG base for code linked in KUSEG: All-Star Baseball 2000's at 0x00100000; N64RX_KUSEG_TABLES=0: not)
        if b is not None and (0x80000000 <= b < 0x80800000 or KUSEG_TABLES and fstart < 0x80000000
                              and 0x10000 <= b < 0x80000000 and abs(b - fstart) < 0x1000000):
            table = (b + lw.simm) & 0xFFFFFFFF
            break
    if table is None:
        return None
    if addr_only:
        return table
    count = None
    v = pc - 4
    while v >= max(fstart, pc - 4 * 48):
        i = Ins(img.word(v), v)
        if i.op == 11:                                                # sltiu: the bound
            count = i.imm
            break
        v -= 4
    if not count:
        return None
    targets = []
    for n in range(count):
        t = img.word(table + 4 * n)
        if t is None or t & 3 or not (fstart <= t < fstart + MAX_FUNC):
            return None
        targets.append(t)
    return table, targets


def walk(img, start, starts):
    """One function from `start`: every instruction its branches reach.
    `starts`: known function starts (a j onto one is a tail call)."""
    f = Func(start, start + 4)
    seen, todo = set(), [start]
    later = set()   # j targets past the end so far: decided once the rest is walked
    while True:
        if not todo:
            # a j forward stays in the function when the function reaches that
            # far anyway: the end is only known once every branch is walked
            # (Mario Golf's 19 KB function jumps 0x4000 ahead before the
            # branches that reach there are walked)
            todo = [t for t in sorted(later) if t not in seen and t < f.end + INTRA_J]
            later.difference_update(todo)
            if not todo:
                break
        pc = todo.pop()
        run = []
        while pc not in seen:
            w = img.word(pc)
            if w is None:
                f.bad = f.bad or f'runs out of the segment at {pc:08X}'
                break
            ins = Ins(w, pc)
            if not ins.valid:
                f.bad = f.bad or f'invalid instruction {w:08X} at {pc:08X}'
                break
            if pc - start > MAX_FUNC or pc < start - MAX_FUNC:
                f.bad = f.bad or 'too large'
                break
            seen.add(pc)
            run.append(ins)
            f.end = max(f.end, pc + 4)
            nxt = pc + 4
            if ins.is_jal or ins.is_branch or ins.is_j or ins.is_jr or ins.is_jalr:
                ds = img.word(nxt)   # the delay slot belongs to the function
                if ds is not None:
                    seen.add(nxt)
                    f.end = max(f.end, nxt + 4)
                    if not (ins.is_jr and not ins.is_jr_ra):
                        run.append(Ins(ds, nxt))   # a jump table's lui can sit in a branch's delay slot
                if ins.is_jal:
                    f.calls.add(ins.target)
                    pc = nxt + 4
                    continue
                if ins.is_link_branch:
                    f.calls.add(ins.branch_target)
                    pc = nxt + 4
                    continue
                if ins.is_branch:
                    todo.append(ins.branch_target)
                    if ins.is_unconditional_branch:
                        break
                    pc = nxt + 4
                    continue
                if ins.is_j:
                    t = ins.target
                    if t not in starts and start <= t < f.end + INTRA_J:
                        todo.append(t)
                    elif t not in starts and t > start and WALK_LATER:
                        later.add(t)
                    else:
                        f.tails.add(t)
                    break
                if ins.is_jr_ra:
                    break
                if ins.is_jr:
                    tab = _table(img, run, len(run) - 1, start) or _table_linear(img, start, pc)
                    if tab:
                        f.jtables.append((pc, tab[0], len(tab[1])))
                        todo.extend(tab[1])
                    else:
                        f.bad = f.bad or f'jr ${ins.rs} at {pc:08X} without a table'
                        t = _table_linear(img, start, pc, addr_only=True)
                        if t and img.word(t) is None and not f.table_want:
                            f.table_want = t   # its bytes in RDRAM no capture holds (romonly.py: want.txt)
                    break
                if ins.is_jalr:
                    pc = nxt + 4
                    continue
            if ins.is_eret:
                break
            pc = nxt
    f.tails.update(later)
    return f


def discover(img, seg, roots, weak=(), weak_calls=False):
    """Every function of `seg` reachable from `roots`, then its gaps, and
    again from what the gaps called, until nothing new turns up. `weak`:
    roots that may be another overlay's call target at the same address; one
    that a function's own branches run past is not a start. `weak_calls`:
    every call target found is weak (code captured from RDRAM: one overlay's
    bytes, where other code jals into the overlays loaded there at other
    times; Banjo-Kazooie's core code calls level functions that sit
    mid-function in its cutscene overlay, which split its functions and
    lost their epilogues)."""
    funcs, starts = {}, set(roots)
    queue = list(roots)
    prev, img.focus = img.focus, seg
    try:
        return _discover(img, seg, roots, funcs, starts, queue, set(weak), weak_calls)
    finally:
        img.focus = prev


def _discover(img, seg, roots, funcs, starts, queue, weak, weak_calls=False):
    strong = set(starts) - weak   # where a walk stops: a weak start is walked over
    while True:
        while queue:
            s = queue.pop()
            if s in funcs or not seg.has(s):
                continue
            f = walk(img, s, strong)
            f.how = 'entry' if s in roots else 'call'
            funcs[s] = f
            for k in [k for k in funcs if funcs[k].how == 'gap' and s < k < f.end]:
                del funcs[k]   # a guess inside a function a call proved
            for t in sorted(f.calls | f.tails):
                if t not in starts:
                    starts.add(t)
                    queue.append(t)
                    if weak_calls and seg.has(t):
                        weak.add(t)
                    else:
                        strong.add(t)
        _settle(funcs, weak)
        new = _gaps(img, seg, funcs)
        _settle(funcs, weak)
        for f in new:
            for t in sorted(f.calls | f.tails):
                if t not in starts:
                    starts.add(t)
                    queue.append(t)
                    if weak_calls and seg.has(t):
                        weak.add(t)
                    else:
                        strong.add(t)
        if not queue and PROBE and not weak_calls:
            for t in _probe(img, seg, funcs):
                if t not in starts:
                    starts.add(t)
                    queue.append(t)
                    strong.add(t)
        if not queue:
            return funcs


# Code the walks never reach: Virtual Pool 64 found 58 of its ~1900 functions, Starshot 77, their
# game code behind a data blob or past the last function found, reached through pointers. In a run
# of the segment no function covers, the jals aimed inside the run that land on a function's start
# (a prologue, addiu sp, sp, -n, or the word after a jr ra and its delay slot: a leaf) are taken as
# starts, when enough of them do: code calling code where it sits (Virtual Pool 64: 1420 of 1480).
# N64RX_PROBE=0: only calls, gaps and roots, as before
PROBE = os.environ.get('N64RX_PROBE', '1') != '0'
PROBE_MIN_RUN = 0x1000     # bytes of unreached segment worth a look
PROBE_MIN_HITS = 8         # jals in the run that land on a prologue in it
PROBE_MIN_SHARE = 0.6      # of the jals aimed inside the run


def _probe(img, seg, funcs):
    """starts in the segment's unreached runs, from the jals inside them (see PROBE)"""
    covered = sorted((f.start, f.end) for f in funcs.values())
    runs, pc = [], seg.vram
    for a, b in covered + [(seg.vram + seg.size, seg.vram + seg.size)]:
        if a - pc >= PROBE_MIN_RUN:
            runs.append((pc, a))
        pc = max(pc, b)
    found = []
    for lo, hi in runs:
        aimed, hits = set(), set()
        for v in range(lo, hi, 4):
            w = img.word(v)
            if w is None or w >> 26 != 3:   # jal
                continue
            t = (v & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if not lo <= t < hi:
                continue
            aimed.add(t)
            p = img.word(t)
            if (p is not None and p >> 16 == 0x27BD and p & 0x8000) or img.word(t - 8) == 0x03E00008:
                hits.add(t)   # a prologue, or right after a jr ra and its delay slot (a leaf's start)
        if len(hits) >= PROBE_MIN_HITS and len(hits) >= PROBE_MIN_SHARE * len(aimed):
            found += sorted(hits)
    return found


def _settle(funcs, weak=()):
    """No overlaps: a function that ran into the next one stops there, or,
    when the next one is a weak root, takes it in."""
    order = sorted(funcs)
    a = order[0] if order else None
    for b in order[1:]:
        if funcs[a].end > b and b in weak and funcs[a].end >= funcs[b].end:
            del funcs[b]
            continue
        if funcs[a].end > b:
            funcs[a].end = b
        a = b


def _dead_epilogue(img, f):
    """Code no branch reaches that only unwinds a frame and returns:
    `lw ra, n(sp)` or `addiu sp, sp, +n` first, at most 12 words, no call.
    IDO leaves one after a loop that never ends (a thread's main loop). A
    bare `jr ra` is not one: it is a getter or an empty function."""
    if f.calls or f.tails or f.end - f.start > 0x30:
        return False
    first = Ins(img.word(f.start), f.start)
    return (first.op == 35 and first.rt == RA and first.rs == 29) or            (first.op == 9 and first.rs == 29 and first.rt == 29 and first.simm > 0)


def _dead_return(img, f, taken):
    """A bare `jr ra; nop` right after another one, that no call, jump, data
    word or lui/addiu pair names: unoptimised IDO code closes every function
    with one, after the return statement's own (Super Mario 64)."""
    refs, unoptimised = taken
    return (unoptimised and f.end - f.start == 8 and img.word(f.start) == 0x03E00008 and
            img.word(f.start + 4) == 0 and img.word(f.start - 8) == 0x03E00008 and f.start not in refs)


def _referenced(img, seg):
    """(every word of the known segments, every j/jal target and every
    lui/addiu (ori) pair's value; whether `seg` is unoptimised code). IDO
    without optimisation closes an if or a return with `b .+8; nop`, which
    the optimiser never leaves: in -O0 code there are about as many as
    there are returns, in optimised code next to none."""
    import struct
    out = set()
    unopt = False
    for s in img.segments:
        data = img.rom[s.rom:s.rom + s.size]
        ws = struct.unpack(f'>{len(data) // 4}I', data[:len(data) // 4 * 4])
        out.update(ws)
        hi = {}
        for w in ws:
            op = w >> 26
            if op in (2, 3):                                  # j / jal
                out.add((s.vram & 0xF0000000) | (w & 0x03FFFFFF) << 2)
            elif op == 15:
                hi[(w >> 16) & 31] = (w & 0xFFFF) << 16
            elif op in (9, 13) and (w >> 21) & 31 in hi:
                lo = w & 0xFFFF
                v = hi[(w >> 21) & 31] + (lo - 0x10000 if op == 9 and lo & 0x8000 else lo)
                out.add(v & 0xFFFFFFFF)
        if s is seg:
            jr = sum(1 for w in ws if w == 0x03E00008)
            b8 = sum(1 for i in range(len(ws) - 1) if ws[i] == 0x10000001 and ws[i + 1] == 0)
            unopt = jr > 0 and b8 >= jr // 4
    return out, unopt


def _gaps(img, seg, funcs):
    """Code between known functions that nothing called: functions reached
    through pointers (threads, callbacks, tables). Leading zero words are
    padding; a gap counts as a function only when its walk is clean.
    Returns the new functions."""
    new, changed = [], True
    taken = None
    while changed:
        changed = False
        order = sorted(funcs)
        # (and the code before the first function: a loaded segment starts with code)
        pairs = ([(None, order[0])] if order and seg.vram < order[0] else []) + list(zip(order, order[1:]))
        if order and seg.code_end > funcs[order[-1]].end:
            pairs.append((order[-1], seg.code_end))   # and up to the end of the text a descriptor gives
        for a, b in pairs:
            pc = funcs[a].end if a is not None else seg.vram
            while pc < b and img.word(pc) == 0:
                pc += 4
            # a data word before a prologue (All-Star Baseball 2000's 0xC000108F, an `ll`, ahead of addiu
            # sp at 0x00112C30): no compiler starts a function with ll/sc or COP2/3 loads and stores, so the
            # function is the prologue's (N64RX_GAP_DATA=0: as before)
            w0, w1 = img.word(pc), img.word(pc + 4)
            if (GAP_DATA and pc + 4 < b and w0 is not None and w1 is not None and (w0 >> 26) in DATA_OPS
                    and w1 >> 16 == 0x27BD and w1 & 0x8000):
                pc += 4
            if pc >= b:
                continue
            f = walk(img, pc, set(funcs))
            if not f.bad and a is not None and f.end <= b and _dead_epilogue(img, f):
                funcs[a].end = f.end   # unreachable tail of the function before (after an endless loop)
                changed = True
                continue
            if not f.bad and a is not None and pc == funcs[a].end and f.end - f.start == 8 and \
                    _dead_return(img, f, taken := taken if taken is not None else _referenced(img, seg)):
                funcs[a].end = f.end   # IDO -O0's closing jr ra after a return that already left
                changed = True
                continue
            if not f.bad and f.end <= b:
                f.how = 'gap'
                funcs[pc] = f
                new.append(f)
                changed = True
    return new
