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
needs the callee to be identified."""
from .funcs import Segment
from .mips import Ins, SP

ARG_REGS = (4, 5, 6, 7)


def _consts(img, f):
    """(jal pc, target, {arg: value}) for every call in f, args by constant
    propagation along the function's straight-line order"""
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
    elif op == 43 and ins.rs == SP:                    # sw reg, off(sp): an outgoing argument
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
    DMA of code. `natural`: the size is the argument right after the ROM and
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
                    if not 0x80000000 <= v < 0x80800000 or img.seg(v):
                        continue
                    after = ORDER[max(pos[kr], pos[kv]) + 1] if max(pos[kr], pos[kv]) + 1 < len(ORDER) else None
                    for kn, n in args.items():
                        if kn in (kr, kv) or not 0x100 <= n <= 0x400000 or r + n > rom_size:
                            continue
                        out.add((r, v, n, pc, kn == after))
    return sorted(out)


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


def find(img, funcs_by_seg, rom_size):
    """New segments: candidates the known code calls into. Each (ROM, vram,
    size) a call offers is tried: the bytes at every call target must look
    like a function start, and every walk from them must be clean; the best
    score wins: a size in its natural argument first, then the share of
    targets that look like starts, then the most targets, then the
    smallest size."""
    from .funcs import discover
    known = [f for fs in funcs_by_seg for f in fs.values()]
    targets = set()
    for f in known:
        targets |= {t for t in f.calls | f.tails if not img.seg(t)}
    allf = {f.start: f for f in known}
    trials = []
    for r, v, n, pc, natural in candidates(img, allf, rom_size):
        inside = sorted(t for t in targets if v <= t < v + n)
        if not inside:
            continue
        seg = Segment(f'seg_{v:08X}', r, v, n)
        img.segments.append(seg)
        score = sum(_starts_like_function(img, t) for t in inside) / len(inside)
        ok = score >= 0.9
        if ok:
            fs = discover(img, seg, inside)
            ok = bool(fs) and not any(f.bad for f in fs.values())
        img.segments.remove(seg)
        if ok:
            trials.append((not natural, -score, -len(inside), n, seg, inside, pc))
    found = []
    for _a, _s, _l, _n, seg, inside, pc in sorted(trials, key=lambda t: t[:4]):
        if any(o.has(seg.vram) or seg.has(o.vram) for o, _i, _p in found):
            continue
        found.append((seg, inside, pc))
    return found
