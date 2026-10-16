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
from dataclasses import dataclass, field

from .mips import Ins, RA

MAX_FUNC = 0x10000          # a function larger than this is a walk gone wrong
INTRA_J = 0x2000            # a j this close and not onto a known start stays in the function


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


class Image:
    """`focus`: the segment being walked; where overlays share an address it
    is the one read (an overlay's code reads its own bytes)"""
    def __init__(self, rom):
        self.rom = rom
        self.segments = []
        self.focus = None
        self.text_ends = {}         # (rom, vram) -> text end, from load descriptors (segments.in_data)

    def seg(self, v):
        if self.focus is not None and self.focus.has(v):
            return self.focus
        for s in self.segments:
            if s.has(v):
                return s
        return None

    def word(self, v):
        s = self.seg(v)
        if not s:
            return None
        o = s.rom + v - s.vram
        if o + 4 > len(self.rom):
            return None
        return int.from_bytes(self.rom[o:o + 4], 'big')


def parse_entry(rom, entry):
    """The SDK entry: lui/addiu t0 = BSS start, lui/ori t1 = BSS size, a clear
    loop, then the stack and a jump to main (jr t2 or j main). Returns
    (bss_start, bss_size, main) or None when the entry is something else."""
    regs, bss, size, main = {}, None, None, None
    for i in range(32):
        o = 0x1000 + 4 * i
        ins = Ins(int.from_bytes(rom[o:o + 4], 'big'), entry + 4 * i)
        d = ins.dest
        if ins.op == 15:   # lui
            regs[ins.rt] = ins.imm << 16
        elif ins.op in (9, 8) and ins.rs in regs:   # addiu / addi
            regs[ins.rt] = (regs[ins.rs] + ins.simm) & 0xFFFFFFFF
        elif ins.op == 13 and ins.rs in regs:      # ori
            regs[ins.rt] = regs[ins.rs] | ins.imm
        elif ins.op == 43 and ins.rt == 0 and bss is None and ins.rs in regs:   # sw zero, 0(t0): the clear loop
            bss = regs[ins.rs]
            size = regs.get(9)
        elif ins.is_jr:
            main = regs.get(ins.rs)
            break
        elif ins.is_j:
            main = ins.target
            break
        elif d is not None and d not in (8, 9, 10, 29):
            regs.pop(d, None)
    if bss is None or main is None:
        return None
    return bss, size or 0, main


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


def _table_linear(img, fstart, pc):
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
        if b is not None and 0x80000000 <= b < 0x80800000:
            table = (b + lw.simm) & 0xFFFFFFFF
            break
    if table is None:
        return None
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
    while todo:
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
                    break
                if ins.is_jalr:
                    pc = nxt + 4
                    continue
            if ins.is_eret:
                break
            pc = nxt
    return f


def discover(img, seg, roots, weak=()):
    """Every function of `seg` reachable from `roots`, then its gaps, and
    again from what the gaps called, until nothing new turns up. `weak`:
    roots that may be another overlay's call target at the same address; one
    that a function's own branches run past is not a start."""
    funcs, starts = {}, set(roots)
    queue = list(roots)
    prev, img.focus = img.focus, seg
    try:
        return _discover(img, seg, roots, funcs, starts, queue, set(weak))
    finally:
        img.focus = prev


def _discover(img, seg, roots, funcs, starts, queue, weak):
    while True:
        while queue:
            s = queue.pop()
            if s in funcs or not seg.has(s):
                continue
            f = walk(img, s, starts)
            f.how = 'entry' if s in roots else 'call'
            funcs[s] = f
            for k in [k for k in funcs if funcs[k].how == 'gap' and s < k < f.end]:
                del funcs[k]   # a guess inside a function a call proved
            for t in sorted(f.calls | f.tails):
                if t not in starts:
                    starts.add(t)
                    queue.append(t)
        _settle(funcs, weak)
        new = _gaps(img, seg, funcs)
        _settle(funcs, weak)
        for f in new:
            for t in sorted(f.calls | f.tails):
                if t not in starts:
                    starts.add(t)
                    queue.append(t)
        if not queue:
            return funcs


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
