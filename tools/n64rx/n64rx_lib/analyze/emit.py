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


def hw_funcs(a: Analysis, live, nm, replaced):
    """the live functions the recompiler would translate that reach a hardware
    register by a constant KSEG1 address (libultra the rules did not name:
    a later osCartRomInit polling PI_STATUS, a raw SI or AI routine; a game's
    own register code): the runtime's interpreter runs them, its loads and
    stores there going to the register model (xbox/src/rt/mmio.c). A function
    the rules named and the runtime replaces stays replaced."""
    return {x: nm[x] for x, f in a.all_funcs().items()
            if x in live and x != a.entry and nm[x] not in replaced and features(a.img, f).hw}


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
