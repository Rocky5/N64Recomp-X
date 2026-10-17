"""The analysis as N64Recomp's input (PLAN.md 3.14): a symbol file, the
recompiler config, and the report a person reads.

Functions are named func_XXXXXXXX except the libultra ones the rules
found. reachable() tells live code from dead: the game's own code reached
from the entry, the main, function addresses in data and in the code,
with the edges out of the functions the runtime replaces cut, so libultra's
internals that only libultra calls are dead. The pipeline leaves out a
dead function the recompiler refuses (an exception handler's eret, a COP0
register it has no model for) and stops on a live one."""
import pathlib
import re

from . import Analysis
from .features import features


def recomp_lists(root):
    """the recompiler's reimplemented and ignored names, read from its source at the pin"""
    src = pathlib.Path(root) / 'tools/recomp/src/symbol_lists.cpp'
    out = {}
    if not src.exists():
        return {'reimplemented_funcs': set(), 'ignored_funcs': set()}
    text = src.read_text(encoding='utf-8', errors='replace')
    for name in ('reimplemented_funcs', 'ignored_funcs'):
        at = text.find(name)
        end = text.find('};', at)
        out[name] = set(re.findall(r'"([^"]+)"', text[at:end])) if at >= 0 else set()
    return out


def names(a: Analysis, ids):
    """func_XXXXXXXX, func_XXXXXXXX_<ROM offset> in an overlay (a segment that
    shares its vram with another), or the libultra name"""
    return {k: (ids[k][0] if k in ids else f'func_{k:08X}' if k < 1 << 32 else f'func_{k & 0xFFFFFFFF:08X}_{k >> 32:X}')
            for k in a.all_funcs()}


def data_pointers(a: Analysis):
    """function starts that appear as words outside every function body, in the
    segments and in the rest of the ROM"""
    funcs = a.all_funcs()
    bv = a.by_vram(funcs)
    starts = set(bv)
    out = set()
    for seg in a.img.segments:
        # this segment's own bodies: an overlay's data may sit where another
        # overlay at the same address has code
        spans = sorted((f.start, f.end) for f in a.funcs.get(seg.name, {}).values())
        o0 = seg.rom
        data = a.img.rom[o0:o0 + seg.size]
        i, k = 0, 0
        while i + 4 <= len(data):
            v = seg.vram + i
            while k < len(spans) and spans[k][1] <= v:
                k += 1
            if k < len(spans) and spans[k][0] <= v < spans[k][1]:
                i = spans[k][1] - seg.vram
                continue
            w = int.from_bytes(data[i:i + 4], 'big')
            if w in starts:
                out.add(w)
            i += 4
    # and the ROM outside every segment: data the game reads at run time
    # (scripts, display-list callbacks, object tables) names functions too
    import struct
    rom = a.img.rom
    covered = sorted((s.rom, s.rom + s.size) for s in a.img.segments)
    pos = 0x1000
    for r0, r1 in covered + [(len(rom) // 4 * 4, len(rom))]:
        if r0 > pos:
            out.update(w for w in struct.unpack(f'>{(r0 - pos) // 4}I', rom[pos:pos + (r0 - pos) // 4 * 4])
                       if w in starts)
        pos = max(pos, r1 + (-r1 & 3))
    return {k for w in out for k in bv[w]}   # an address in several overlays: each one's function


def targets(a: Analysis, funcs, bv, k, vs):
    """the keys the vram targets `vs` of function k name: the function in k's
    own segment when it has one there, else every overlay's at that address"""
    hi = k >> 32
    seg = a.seg_of(k) if hi else None
    out = set()
    for t in vs:
        ks = bv.get(t, ())
        if len(ks) > 1 and seg is not None and seg.has(t):
            own = [x for x in ks if x >> 32 == seg.rom + t - seg.vram]
            ks = own or ks
        out.update(ks)
    return out


def reachable(a: Analysis, nm, replaced):
    funcs = a.all_funcs()
    bv = a.by_vram(funcs)
    prev = a.img.focus
    feats = {}
    for x, f in funcs.items():
        a.img.focus = a.seg_of(x) if x >> 32 else None
        feats[x] = features(a.img, f)
    a.img.focus = prev
    roots = {a.entry, a.main} | data_pointers(a)
    seen, todo = set(), [r for r in roots if r in funcs]
    while todo:
        x = todo.pop()
        if x in seen:
            continue
        seen.add(x)
        if nm[x] in replaced:
            continue
        f = funcs[x]
        for t in targets(a, funcs, bv, x, f.calls | f.tails | (feats[x].addr_taken & set(bv))):
            if t not in seen:
                todo.append(t)
    return seen


def _size(a, seg, f):
    """a function never ends on a jump or branch without its delay slot (a walk
    the next function's start cut: Toy Story 2's call that falls into the next
    function); the recompiler needs the slot to place what follows the call"""
    from .mips import Ins
    end = f.end
    o = seg.rom + (end - 4 - seg.vram)
    if end - 4 >= f.start and o + 8 <= len(a.img.rom):
        ins = Ins(int.from_bytes(a.img.rom[o:o + 4], 'big'), end - 4)
        if ins.is_jal or ins.is_j or ins.is_jr or ins.is_jalr or ins.is_branch:
            end += 4
    return end - f.start


def write_syms(a: Analysis, nm, path):
    lines = ['# Generated by n64rx from the ROM alone (PLAN.md 3.14): addresses, sizes, names.',
             '# Derived from your ROM on your machine; never commit it.', '']
    ovl = a.overlays()
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        lines += ['[[section]]', f'name = ".{seg.name}"', f'rom = 0x{seg.rom:08X}', f'vram = 0x{seg.vram:08X}',
                  f'size = 0x{seg.size:X}', '', 'functions = [']
        for x in sorted(fs):
            lines.append(f'    {{ name = "{nm[a.key(seg, x, ovl)]}", vram = 0x{x:08X}, size = 0x{_size(a, seg, fs[x]):X} }},')
        lines += [']', '']
    pathlib.Path(path).write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def _reg_or_probe(ft):
    """a register by a fixed address, or RDRAM by a fixed uncached one (a game's
    own memory-size probe at 0xA0400000: Ocarina of Time, F-Zero X, Bassmasters;
    the interpreter's model reads past RDRAM as nothing, recompiled code faults)"""
    return bool(ft.hw) or ft.k1ram or any(0xA0000000 <= c < 0xA0800000 and not c & 0xFFFF for c in ft.consts)
    # (the upper half of an uncached RDRAM address formed: Ocarina of Time's -O0 check keeps the
    # address on the stack, out of the constant tracker's reach; a handful of functions a game)


def hw_funcs(a: Analysis, live, nm, replaced):
    """the live functions the recompiler would translate that reach a hardware
    register by a constant KSEG1 address (libultra the rules did not name:
    a later osCartRomInit polling PI_STATUS, a raw SI or AI routine; a game's
    own register code): the runtime's interpreter runs them, its loads and
    stores there going to the register model (xbox/src/rt/mmio.c). A function
    the rules named and the runtime replaces stays replaced."""
    return {x: nm[x] for x, f in a.all_funcs().items()
            if x in live and x != a.entry and nm[x] not in replaced and _reg_or_probe(features(a.img, f))}


def report(a: Analysis, ids, live, nm, replaced):
    funcs = a.all_funcs()
    out = ['n64rx: what the analysis found (PLAN.md 3.14)', '']
    out += a.notes + ['']
    out.append(f'{len(funcs)} functions, {len(live)} live, {len(ids)} named as libultra:')
    for x, (n, why) in sorted(ids.items(), key=lambda t: t[1][0]):
        out.append(f'  {x:08X} {n:28} {why}')
    missing = sorted(n for n in replaced if n.startswith(('os', '__os')) and n not in set(nm.values()))
    out += ['', 'libultra the runtime replaces that this ROM was not found to have (fine when the game never uses it):',
            '  ' + ', '.join(missing), '']
    bad = [f for f in funcs.values() if f.bad]
    if bad:
        out.append('walks that stopped early (their end may be short):')
        out += [f'  {nm[k]}: {f.bad}' for k, f in funcs.items() if f.bad]
    return '\n'.join(out) + '\n'


def cross_funcs(a: Analysis, live, nm, replaced):
    """live functions with a branch or j into the body of another function
    (not its start), or a branch to code no function holds: the recompiler emits a goto to a label the other
    function holds, which C cannot compile ("use of undeclared label"; the
    library sweep's 12 games: a shared tail, a function whose bounds the
    walk split). The runtime's interpreter runs them; the target function
    stays recompiled for its own callers."""
    from .mips import Ins, words
    out = {}
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        spans = sorted((f.start, f.end) for f in fs.values())
        starts = [s for s, _ in spans]
        import bisect
        for x, f in fs.items():
            if x not in live or x == a.entry or nm[x] in replaced:
                continue
            n = (f.end - f.start) // 4
            ws = words(a.img.rom, seg.rom + (f.start - seg.vram), n)
            for i, w in enumerate(ws):
                ins = Ins(w, f.start + 4 * i)
                if ins.is_branch and not ins.is_link_branch:
                    t = ins.branch_target
                elif ins.is_j:
                    t = ins.target
                else:
                    continue
                if f.start <= t < f.end:
                    continue
                k = bisect.bisect_right(starts, t) - 1
                inside = k >= 0 and spans[k][0] < t < spans[k][1]
                start = k >= 0 and spans[k][0] == t
                if inside or (not start and ins.is_branch and seg.has(t)):
                    # into another function's body, or a conditional branch out to
                    # code no function holds (the recompiler only turns a j or
                    # an unconditional branch to a start into a tail call)
                    out[x] = nm[x]
                    break
    return out


def _n64recomp_tables(ws, vram):
    """the jump tables N64Recomp's analysis finds in a function (its
    analysis.cpp, a straight pass over the words: lui, one addiu, an addu of
    an index, lw, jr), as table vrams"""
    from .mips import Ins
    fresh = lambda: {'lui': None, 'addiu': None, 'addend': False, 'loaded': None}
    regs = [fresh() for _ in range(32)]
    stack, out = {}, []
    for i, w in enumerate(ws):
        ins = Ins(w, vram + 4 * i)
        op, rs, rt, rd = ins.op, ins.rs, ins.rt, ins.rd
        if op == 15:                                         # lui
            regs[rt] = fresh()
            regs[rt]['lui'] = (ins.simm << 16) & 0xFFFFFFFF
        elif op == 9:                                        # addiu
            st = dict(regs[rs])
            if st['addiu'] is None:
                st['addiu'] = ins.simm
            else:
                st = fresh()
            regs[rt] = st
        elif op == 0 and ins.funct == 33:                    # addu: a lui register plus another
            lr, ar = (regs[rs]['lui'] is not None), (regs[rt]['lui'] is not None)
            if lr != ar:
                st = dict(regs[rs] if lr else regs[rt])
                st['addend'] = True
            else:
                st = fresh()
            regs[rd] = st
        elif op == 0 and ins.funct in (45, 37):              # daddu, or: a move copies
            regs[rd] = dict(regs[rt]) if rs == 0 else dict(regs[rs]) if rt == 0 else fresh()
        elif op == 43 and rs == 29:                          # sw to the stack
            stack[ins.imm] = dict(regs[rt])
        elif op == 35:                                       # lw
            st = fresh()
            if rs == 29:
                st = dict(stack.get(ins.imm, fresh()))
            elif regs[rs]['lui'] is not None and regs[rs]['addend']:
                if not (ins.imm and regs[rs]['addiu'] is not None):
                    lo = ins.simm if ins.imm else (regs[rs]['addiu'] or 0)
                    st['loaded'] = (regs[rs]['lui'] + lo) & 0xFFFFFFFF
            regs[rt] = st
        elif ins.is_jr and rs != 31:
            if regs[rs]['loaded'] is not None:
                out.append(regs[rs]['loaded'])
        elif ins.dest is not None:
            regs[ins.dest] = fresh()
    return sorted(out)


def recomp_hazards(a: Analysis, live, nm, replaced):
    """live functions N64Recomp would crash on or refuse one run at a time:
    a jump table it reads outside the function's own segment (it takes the
    table to sit in the function's segment and reads the ROM past its end:
    a segfault, Conker's TLB-mapped code with its tables in KSEG0), and a
    jal to an address where no segment has a function and that is not in the
    caller's own segment ("No function found for jal target": Banjo-Kazooie's
    captured code, ~150 callers, one recompiler run each). The runtime's
    interpreter runs them; also a cop0 register other than Status, which
    N64Recomp refuses. Returns {vram: (name, why, want)}: `want`, the RDRAM
    range from the function to its table when the function is captured code
    (learn.py): the console saves it, the next build has both in one segment"""
    from .mips import Ins, words
    rom = a.img.rom
    starts = {x for fs in a.funcs.values() for x in fs}
    out = {}
    for seg in a.img.segments:
        for x, f in a.funcs.get(seg.name, {}).items():
            if x not in live or nm[x] in replaced:
                continue
            n = (f.end - f.start) // 4
            off = seg.rom + (f.start - seg.vram)
            ws = words(rom, off, n)
            why, want = None, None
            for i, w in enumerate(ws):
                ins = Ins(w, f.start + 4 * i)
                if ins.is_jal and not seg.has(ins.target) and ins.target not in starts:
                    why = f'jal {ins.target:08X}: no function there'
                    break
                if ins.is_jal and seg.name.startswith('code_') and seg.has(ins.target) and ins.target not in starts:
                    # into captured code where no function starts: another overlay's function at that address
                    # (N64Recomp would make a static there and split the function that holds it)
                    why = f'jal {ins.target:08X}: another overlay\'s function'
                    break
                if ins.op == 17 and ins.rs in (16, 17) and 8 <= ins.funct <= 11:   # round/trunc/ceil/floor.l: not in N64Recomp
                    why = f'{("round", "trunc", "ceil", "floor")[ins.funct - 8]}.l.{"s" if ins.rs == 16 else "d"}'
                    break
                if ins.op == 16 and ins.rs in (0, 4) and ins.rd != 12:   # mfc0/mtc0: N64Recomp takes Status only
                    why = f'{"mfc0" if ins.rs == 0 else "mtc0"} of cop0 register {ins.rd}'
                    break
                if (ins.is_branch and not ins.is_link_branch) or ins.is_j:
                    t = ins.target if ins.is_j else ins.branch_target
                    if not seg.has(t) and t not in starts:   # past the segment: a label C cannot have
                        why = f'branch to {t:08X}, past its segment'
                        if seg.name.startswith('code_') and 0x80000000 <= f.start < t < 0x80800000 and t - f.start <= 0x100000:
                            want = (f.start, t + 4)   # the rest of the function, in RDRAM no capture holds
                        break
            if why is None:
                for t in _n64recomp_tables(ws, f.start):
                    r = (t - f.start + off) & 0xFFFFFFFF
                    if not seg.has(t) or r + 4 > len(rom):
                        why = f'jump table at {t:08X}, outside its segment'
                        lo, hi = min(f.start, t), max(f.end, t + 4)
                        if seg.name.startswith('code_') and 0x80000000 <= lo and hi <= 0x80800000 and hi - lo <= 0x100000:
                            want = (lo, hi)
                        break
            if why:
                out[x] = (nm[x], why, want)
    return out
