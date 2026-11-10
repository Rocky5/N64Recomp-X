"""The analysis as N64Recomp's input (PLAN.md 3.14): a symbol file, the
recompiler config, and the report a person reads.

Functions are named func_XXXXXXXX except the libultra ones the rules
found. reachable() tells live code from dead: the game's own code reached
from the entry, the main, function addresses in data and in the code,
with the edges out of the functions the runtime replaces cut, so libultra's
internals that only libultra calls are dead. The pipeline leaves out a
dead function the recompiler refuses (an exception handler's eret, a COP0
register it has no model for) and stops on a live one."""
import os
import pathlib
import re

from . import Analysis
from .features import features

# kill switch: N64RX_XJAL_INTERP=0 leaves a function with a jal into another's body recompiled
XJAL = os.environ.get('N64RX_XJAL_INTERP', '1') != '0'
# kill switch: N64RX_XBAL=0 leaves a bal/bgezal/bltzal into a function's body to N64Recomp's static
XBAL = os.environ.get('N64RX_XBAL', '1') != '0'
# patches/recomp/0002: a jal to an address where this segment has no function (another overlay's, a
# function's body, or code no segment holds) is called through the runtime's lookup instead of
# interpreting its caller. N64RX_LOOKUP_JAL=0: the caller interpreted, as before
LOOKUP_JAL = os.environ.get('N64RX_LOOKUP_JAL', '1') != '0'
# A jal from outside to a function of an overlay another shares the address of: whichever is loaded when it runs. With one
# function at that address the recompiler bound the call to it, whatever RDRAM held (Fighter's Destiny's boot code ran
# one overlay's function with another's loaded, and wrote through a ROM offset). N64RX_OVL_LOOKUP=0: bound, as before
OVL_LOOKUP = os.environ.get('N64RX_OVL_LOOKUP', '1') != '0'
# In a ROM with an entry hook (a ROM hack, funcs.hook_copies), a `j` out of a function to where no function starts in its
# own segment is a hook: the hack's code in a gap of the segment, which jumps back into the middle of the function. WWF No
# Mercy Plus V3 stores a pointer at 0x804000F0 from 0x800430C0 (a gap in the boot segment, reached from 0x80038240 and back
# to 0x80038134); recompiled, the jump was a tail call, the store never ran and an overlay wrote through 0xFFFF8864. The
# interpreter runs such a function and follows the jumps in RDRAM. N64RX_HOOK_J=0: recompiled, as before
HOOK_J = os.environ.get('N64RX_HOOK_J', '1') != '0'
# In such a ROM, a `j` out of a function after a lui/addiu (or ori) that built an address in the rest of the function: a
# call that returns through that register, not a tail call. WWF No Mercy Plus V3's Create-a-Wrestler menu, 0x80450D34, sets
# $fp to 0x80450DB0 (just past the j, where the analysis ended the function) and jumps to 0x804500C4, which ends `move $ra, $fp; jr $ra`; recompiled, the function returned there
# without its epilogue, 0x2A0 bytes down the stack, and its caller returned to a float (0xBDBAF3A3). The interpreter runs
# such a function. N64RX_HOOK_JRET=0: recompiled, as before
HOOK_JRET = os.environ.get('N64RX_HOOK_JRET', '1') != '0'
# captured code's jump tables in KUSEG pages, and its callees in pages, no capture holds: asked for
# (N64RX_KUSEG_WANT=0: not)
KUSEG_WANT = os.environ.get('N64RX_KUSEG_WANT', '1') != '0'


def recomp_lists(root):
    """the recompiler's reimplemented and ignored names, read from its source at the pin"""
    src = pathlib.Path(root) / 'tools/recomp/src/symbol_lists.cpp'
    if not src.exists() and os.environ.get('N64RX_RECOMP'):   # the app's toolchain bundle: a copy beside its recompiler
        src = pathlib.Path(os.environ['N64RX_RECOMP']) / 'symbol_lists.cpp'
    out = {}
    if not src.exists():   # without them the config names functions N64Recomp then refuses (a worktree with no submodule)
        from .. import N64rxError
        raise N64rxError("The recompiler's source (tools/recomp) is missing: its symbol lists decide what N64Recomp skips.",
                         'run: git submodule update --init tools/recomp')
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


def reachable(a: Analysis, nm, replaced, extra_roots=()):
    """`extra_roots`: functions reached by other means (relocatable overlays: through pointers in
    their own data once the game relocates them), whose callees are live too"""
    funcs = a.all_funcs()
    bv = a.by_vram(funcs)
    prev = a.img.focus
    feats = {}
    for x, f in funcs.items():
        a.img.focus = a.seg_of(x) if x >> 32 else None
        feats[x] = features(a.img, f)
    a.img.focus = prev
    roots = {a.entry, a.main} | data_pointers(a) | set(extra_roots)
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


def _end(a, seg, f):
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
    return end


def _falls_through(a, seg, start, end):
    """the path runs off the function's end: its last instruction, or the one
    whose delay slot that is, is not a jr, a j or an unconditional branch"""
    from .mips import Ins

    def at(v):
        o = seg.rom + (v - seg.vram)
        return Ins(int.from_bytes(a.img.rom[o:o + 4], 'big'), v) if seg.vram <= v and o + 4 <= len(a.img.rom) else None

    for v in (end - 8, end - 4):
        ins = at(v) if v >= start else None
        if ins and ((ins.is_jr and not ins.is_jalr) or ins.is_j or ins.is_unconditional_branch or ins.is_eret):
            return False
    return True


# A branch onto the delay slot of the jump that ends a function: the CPU runs the slot and goes on past
# the end (Donkey Kong 64's hand-written 0x806195D0: a beqz to the slot of its loop's j at 0x8061A754,
# then on into 0x8061A774's body and the routine's shared exit). The function ended at the slot and the
# recompiled path returned from there, without the exit that restores $sp, Status.FR and $f20-$f31: its
# caller read its saved $a0 from the wrong frame (N64RX_HANDWRITTEN_SPAN=1 crashed at frame 69). The
# function takes the code after the slot, up to the next function's start (then the fall-through rule)
# or the next jump's delay slot. N64RX_SLOT_ENTRY=0: as before.
SLOT_ENTRY = os.environ.get('N64RX_SLOT_ENTRY', '1') != '0'


def _slot_entered(a, seg, start, end):
    """a branch in [start, end) lands on the last instruction, the delay slot of the jump that ends the path"""
    from .mips import Ins, words
    if end - 8 < start:
        return False
    o = seg.rom + (end - 8 - seg.vram)
    last = Ins(int.from_bytes(a.img.rom[o:o + 4], 'big'), end - 8)
    if not ((last.is_jr and not last.is_jalr) or last.is_j or last.is_unconditional_branch):
        return False
    for i, w in enumerate(words(a.img.rom, seg.rom + (start - seg.vram), (end - 8 - start) // 4)):
        ins = Ins(w, start + 4 * i)
        if ins.is_branch and not ins.is_link_branch and ins.branch_target == end - 4:
            return True
    return False


def _run_on(a, seg, fs, v, limit):
    """where the code from v (past a function's end) stops: the next function's start, or past the delay
    slot of a jump, a return or an unconditional branch"""
    from .mips import Ins
    while v < limit and seg.has(v + 4):
        g = fs.get(v)
        if g is not None and g.start == v:
            return v
        o = seg.rom + (v - seg.vram)
        ins = Ins(int.from_bytes(a.img.rom[o:o + 4], 'big'), v)
        if (ins.is_jr and not ins.is_jalr) or ins.is_j or ins.is_unconditional_branch or ins.is_eret:
            return v + 8
        v += 4
    return v


def _size(a, seg, f, fs=None):
    """the bytes the recompiler translates for f. A function whose path runs
    off its end into the next one (GoldenEye's hand-written cosf: three
    instructions, then sinf) takes the next one's body too, up to where a path
    ends, so the recompiled cosf goes on into sinf as the CPU does; it ended
    at sinf's start and returned with $f0 unset (the Dam's sky and far
    geometry, the pause watch's model). N64RX_FALLTHROUGH=0: as before."""
    end = _end(a, seg, f)
    for _ in range(8):   # each rule can open the way for the other
        was = end
        if fs and os.environ.get('N64RX_FALLTHROUGH', '1') != '0':
            end = _follow(a, seg, f, fs, end)
        if fs and HANDWRITTEN_SPAN:
            end = _branch_span(a, seg, f, fs, end)
        if end == was:
            break
    return end - f.start


def _follow(a, seg, f, fs, end):
    """f's end moved on over the paths that run past it"""
    for _ in range(8):   # a chain of fall-throughs, bounded
        nxt = fs.get(end)
        if nxt is not None and nxt.start == end and (_falls_through(a, seg, f.start, end) or
                                                    SLOT_ENTRY and _slot_entered(a, seg, f.start, end)):
            end = _end(a, seg, nxt)
            continue
        if SLOT_ENTRY and _slot_entered(a, seg, f.start, end):
            on = _run_on(a, seg, fs, end, f.start + SPAN_MAX)
            if on > end:
                end = on
                continue
        break
    return end


# A branch (not a j, not a call) to the start of a later function is hand-written code with several
# entries: Donkey Kong 64's 0x806195D0 (Rare's, Status.FR set inside) branches to a shared exit at
# 0x8061A2A8, past four entries the dry boot learnt from their jals; cut at the first, it was
# interpreted whole (cross_funcs: 14 million instructions every 5 s in xemu). Its range takes those
# functions' bodies too, up to SPAN_MAX past its start; they stay functions of their own for their
# callers. A compiler's branch never lands on another function's start (its tail calls are j).
# DK64's routine crashed the game at frame 69 until SLOT_ENTRY let its range run on past the branch onto a
# delay slot to its shared exit. On (N64RX_HANDWRITTEN_SPAN=0: interpreted, as before) after the sweep
# sspan (xemu, 22 games, s16's settings): no new crash; DK64 2,267 -> 5,827 frames in 120 s, Turok 1's
# crash gone, Turok 2 and 3 a third more frames; Mario Party's board the same, shot for shot
HANDWRITTEN_SPAN = os.environ.get('N64RX_HANDWRITTEN_SPAN', '1') != '0'
SPAN_MAX = 0x4000


def _branch_span(a, seg, f, fs, end):
    from .mips import Ins, words
    for _ in range(16):
        ws = words(a.img.rom, seg.rom + (f.start - seg.vram), (end - f.start) // 4)
        far = end
        for i, w in enumerate(ws):
            ins = Ins(w, f.start + 4 * i)
            if not ins.is_branch or ins.is_link_branch:
                continue
            t = ins.branch_target
            g = fs.get(t)
            if t >= end and g is not None and g.start == t and t - f.start <= SPAN_MAX:
                far = max(far, _end(a, seg, g))
        if far == end or far - f.start > SPAN_MAX or not seg.has(far - 4):
            break
        end = far
    return end


def write_syms(a: Analysis, nm, path):
    lines = ['# Generated by n64rx from the ROM alone (PLAN.md 3.14): addresses, sizes, names.',
             '# Derived from your ROM on your machine; never commit it.', '']
    ovl = a.overlays()
    for seg in getattr(a, 'table_sources', []):   # learn.py: RDRAM for jump tables, read first (patches/recomp/0001)
        lines += ['[[section]]', f'name = ".{seg.name}"', f'rom = 0x{seg.rom:08X}', f'vram = 0x{seg.vram:08X}',
                  f'size = 0x{seg.size:X}', '', 'functions = [', ']', '']
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        lines += ['[[section]]', f'name = ".{seg.name}"', f'rom = 0x{seg.rom:08X}', f'vram = 0x{seg.vram:08X}',
                  f'size = 0x{seg.size:X}', '', 'functions = [']
        for x in sorted(fs):
            # no further than the image: a captured page's last function that goes on into the next page (Indiana
            # Jones's TLB-mapped code from the dry boot, the last block appended) made N64Recomp refuse the whole
            # symbol file ("out of bounds of the provided rom"); the rest as before
            size = min(_size(a, seg, fs[x], fs), len(a.img.rom) - (seg.rom + x - seg.vram))
            if size > 0:
                lines.append(f'    {{ name = "{nm[a.key(seg, x, ovl)]}", vram = 0x{x:08X}, size = 0x{size:X} }},')
        lines += [']', '']
        if getattr(seg, 'relocs', None):   # a relocatable overlay (zovl.py): N64Recomp reads RELOC_HI16/LO16 for these
            lines += ['relocs = [']
            lines += [f'    {{ vram = 0x{v:08X}, target_vram = 0x{t:08X}, type = "{ty}" }},' for v, t, ty in seg.relocs]
            lines += [']', '']
    pathlib.Path(path).write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def _reg_or_probe(ft):
    """a register by a fixed address, or RDRAM by a fixed uncached one (a game's
    own memory-size probe at 0xA0400000: Ocarina of Time, F-Zero X, Bassmasters;
    the interpreter's model reads past RDRAM as nothing, recompiled code faults)"""
    # a probe's address is a MB boundary; a display list's words formed by lui are not (Smash
    # Bros.' overlay builders: 0xA0780000, read as uncached RDRAM once its overlays were read
    # focused). N64RX_PROBE_MB=0: any 64 KB boundary, as before
    step = 0xFFFFF if os.environ.get('N64RX_PROBE_MB', '1') != '0' else 0xFFFF
    return bool(ft.hw) or ft.k1ram or any(0xA0000000 <= c < 0xA0800000 and not c & step for c in ft.consts)
    # (the upper half of an uncached RDRAM address formed: Ocarina of Time's -O0 check keeps the
    # address on the stack, out of the constant tracker's reach; a handful of functions a game)


def hw_funcs(a: Analysis, live, nm, replaced):
    """the live functions the recompiler would translate that reach a hardware
    register by a constant KSEG1 address (libultra the rules did not name:
    a later osCartRomInit polling PI_STATUS, a raw SI or AI routine; a game's
    own register code): the runtime's interpreter runs them, its loads and
    stores there going to the register model (xbox/src/rt/mmio.c). A function
    the rules named and the runtime replaces stays replaced. An overlay's
    function is read with the image's focus on its segment (as reachable()
    does): unfocused, the words at its address were another overlay's or none,
    and one reading SP_STATUS stayed recompiled (review 4.3).
    N64RX_HW_FOCUS=0: read unfocused, as before"""
    focus = os.environ.get('N64RX_HW_FOCUS', '1') != '0'
    prev, out = a.img.focus, {}
    try:
        for x, f in a.all_funcs().items():
            if x not in live or x == a.entry or nm[x] in replaced:
                continue
            a.img.focus = a.seg_of(x) if focus and x >> 32 else prev
            if _reg_or_probe(features(a.img, f)):
                out[x] = nm[x]
    finally:
        a.img.focus = prev
    return out


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


def cross_funcs(a: Analysis, live, nm, replaced, lookups=None):
    """live functions with a branch or j into the body of another function
    (not its start), or a branch to code no function holds: the recompiler emits a goto to a label the other
    function holds, which C cannot compile ("use of undeclared label"; the
    library sweep's 12 games: a shared tail, a function whose bounds the
    walk split). The runtime's interpreter runs them; the target function
    stays recompiled for its own callers."""
    from .mips import Ins, words
    out = {}
    ovl = a.overlays()
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        spans = sorted((f.start, f.end) for f in fs.values())
        starts = [s for s, _ in spans]
        import bisect
        for x, f in fs.items():
            # an overlay's functions are keyed by their ROM too (a.key): checked by vram
            # alone they were never live here, and Ocarina of Time's learnt overlays with
            # cross branches reached the recompiler. N64RX_XBRANCH_OVL=0: as before
            kx = a.key(seg, x, ovl) if os.environ.get('N64RX_XBRANCH_OVL', '1') != '0' else x
            if kx not in live or x == a.entry or nm.get(kx) in replaced:
                continue
            # the range the recompiler translates (_size: with a fallen-into
            # neighbour's body), scanned whole: a branch out of that body is
            # one out of this function
            end = f.start + _size(a, seg, f, fs)
            n = (end - f.start) // 4
            ws = words(a.img.rom, seg.rom + (f.start - seg.vram), n)
            for i, w in enumerate(ws):
                ins = Ins(w, f.start + 4 * i)
                if ins.is_branch and not ins.is_link_branch:
                    t = ins.branch_target
                elif ins.is_j:
                    t = ins.target
                elif (ins.op == 3 or (ins.is_link_branch and XBAL)) and XJAL:
                    # a jal into another function's body: N64Recomp makes the target a
                    # static function running to the next symbol, whose branches past it
                    # are labels it lacks (Mario Tennis's captured overlays, pages of two
                    # scenes at one vram). N64RX_XJAL_INTERP=0: as before. A bal/bgezal/
                    # bltzal is a call too, and N64Recomp makes a static at its target the
                    # same way (Resident Evil 2's and both Ken Griffey games' hand-written
                    # routine: a bal at 0x800B66B8 into the body of 0x800B682C). That code
                    # also reads ra as a value after a bltzal $zero (addiu t3, ra, 0x184),
                    # which compiled code never sets: the caller is interpreted, not called
                    # through the lookup. N64RX_XBAL=0: not checked
                    t = ins.branch_target if ins.is_link_branch else ins.target
                    k = bisect.bisect_right(starts, t) - 1
                    inside = k >= 0 and spans[k][0] < t < spans[k][1]
                    # or, in code captured from RDRAM, to no function at all: one version's
                    # call where that version has no start (learn._plausible kept another's out)
                    loose = seg.name.startswith('code_') and not (k >= 0 and spans[k][0] == t)
                    if seg.has(t) and (inside or loose):
                        if LOOKUP_JAL and lookups is not None and not ins.is_link_branch:
                            lookups.add(t)   # patches/recomp/0002: whatever the game has there when it runs
                            continue
                        out[kx] = nm[kx]
                        break
                    continue
                else:
                    continue
                if f.start <= t < end:
                    continue
                k = bisect.bisect_right(starts, t) - 1
                inside = k >= 0 and spans[k][0] < t < spans[k][1]
                start = k >= 0 and spans[k][0] == t
                cond = ins.is_branch and not ins.is_unconditional_branch
                if inside or (not start and ins.is_branch and seg.has(t)) or                         (cond and start and os.environ.get('N64RX_XBRANCH_COND', '1') != '0'):
                    # into another function's body, or a conditional branch out to
                    # code no function holds or to another function's start: the
                    # recompiler only turns a j or an unconditional branch to a
                    # start into a tail call, a conditional one stays a goto
                    # (Ocarina of Time's learnt overlays: "use of undeclared label")
                    out[kx] = nm[kx]
                    break
    return out


def _reads_ra(ws, start):
    """whether code reads $ra as a value before anything writes it: an
    offset from it, a load through it, arithmetic on it (Jet Force Gemini's
    call-site resolver finds the jal that called it at ra - 8). jr $ra, a
    store of it and a plain move do not count"""
    from .mips import Ins, RA, LOADS
    for i, w in enumerate(ws):
        ins = Ins(w, start + 4 * i)
        if ins.op in (8, 9, 24, 25) and ins.rs == RA and ins.simm and ins.rt != RA:   # addi(u)/daddi(u) ra, k
            return True
        if ins.op in LOADS and ins.rs == RA:
            return True
        if ins.op == 0 and ins.funct in (34, 35, 42, 43, 46, 47) and RA in (ins.rs, ins.rt):   # sub, slt
            return True
        if ins.op == 0 and ins.funct in (32, 33, 36, 37, 38, 39, 44, 45) and RA in (ins.rs, ins.rt) and                 0 not in (ins.rs, ins.rt):   # add/and/or/xor of ra with another register (not a move)
            return True
        if ins.is_jal or ins.is_jalr or ins.is_link_branch or ins.dest == RA or ins.is_jr_ra:
            return False
    return False


def ra_hooks(a: Analysis, live, nm, replaced, interp):
    """functions that read $ra as a value (_reads_ra) and the jal sites in
    recompiled code that call them: N64Recomp's generated code never sets
    ctx->r31 at a call, so each such site gets a hook that does (before the
    jal, its return address), and an interpreted one enters the interpreter
    with that ra instead of its sentinel (xrc_interp_ra). Returns ({key:
    name} of the readers, [(func, site vram, return address)])"""
    from .mips import Ins, words
    ovl = a.overlays()
    readers, rows = {}, []
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        for x, f in fs.items():
            kx = a.key(seg, x, ovl)
            if kx not in live or nm.get(kx) in replaced:
                continue
            ws = words(a.img.rom, seg.rom + (f.start - seg.vram), min(64, (f.end - f.start) // 4))
            if _reads_ra(ws, f.start):
                readers[kx] = nm[kx]
    if not readers:
        return readers, rows
    want = {k & 0xFFFFFFFF for k in readers}
    for seg in a.img.segments:
        fs = a.funcs.get(seg.name, {})
        for x, f in fs.items():
            kx = a.key(seg, x, ovl)
            if kx not in live or kx in interp or nm.get(kx) in replaced or x == a.entry:
                continue
            n = _size(a, seg, f, fs) // 4
            for i, w in enumerate(words(a.img.rom, seg.rom + (f.start - seg.vram), n)):
                ins = Ins(w, f.start + 4 * i)
                if ins.is_jal and ins.target in want:
                    rows.append((nm[kx], ins.pc, ins.pc + 8))
    return readers, rows


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


def _table_elsewhere(a, rom, t, f):
    """a jump table outside the function's segment that N64Recomp (with
    patches/recomp/0001) reads from the first segment holding its vram, as
    it does: there in the ROM, its first entry inside the function (else its
    analysis fails on the table: "Failed to determine size")"""
    import struct
    holders = [o for o in getattr(a, 'table_sources', []) + list(a.img.segments) if o.has(t)]   # write_syms' order
    # the recompiler reads the first holder: only an RDRAM table section, or
    # the one segment there is (overlays sharing the vram: Mario Party's
    # tables came out of another overlay and its switches jumped wild)
    if not holders or (len(holders) > 1 and not holders[0].name.startswith('rdram_')):
        return False
    for o in holders[:1]:
        r = o.rom + (t - o.vram)
        if r < 0 or r + 4 > len(rom):
            return False
        w = struct.unpack('>I', rom[r:r + 4])[0]
        return f.start <= w < f.end
    return False


def _hook_jumps_back(rom, seg, t, starts, memo, limit=4096):
    """the hack's code a jal lands on at t (inside `seg`, the code its entry hook copies) can `j` out of `seg` to where no
    function starts: back into the middle of its caller, as a hook does instead of returning. A flood over its
    instructions: branches and jumps inside `seg` are followed, a `jr` or a jump to a known start ends a path"""
    import struct
    from .mips import Ins
    if t in memo:
        return memo[t]
    seen, todo, back = set(), [t], False
    while todo and not back and len(seen) < limit:
        v = todo.pop()
        while seg.has(v) and v not in seen and len(seen) < limit:
            seen.add(v)
            o = seg.rom + (v - seg.vram)
            if o < 0 or o + 4 > len(rom):
                break
            ins = Ins(struct.unpack('>I', rom[o:o + 4])[0], v)
            if ins.is_j:
                if seg.has(ins.target):
                    todo.append(ins.target)
                elif ins.target not in starts:
                    back = True
                break
            if ins.is_jr:
                break
            if ins.is_branch and not ins.is_link_branch:
                todo.append(ins.branch_target)
            v += 4
    memo[t] = back
    return back


def recomp_hazards(a: Analysis, live, nm, replaced, lookups=None):
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
    (learn.py): the console saves it, the next build has both in one segment.
    With LOOKUP_JAL the two jal cases are not hazards: their targets go into
    `lookups` (a set), for the recompiler to call through the lookup."""
    from .mips import Ins, words
    rom = a.img.rom
    starts = {x for fs in a.funcs.values() for x in fs}   # vram: N64Recomp finds a jal's function in every section
    ovl = a.overlays()
    hooked = HOOK_J and any(s.name.startswith('hook_') for s in a.img.segments)   # a ROM hack's entry hook (funcs.hook_copies)
    hook_segs = [s for s in a.img.segments if s.name.startswith('hook_')]
    jumps_back = {}   # _hook_jumps_back's answers, by address
    out = {}
    for seg in a.img.segments:
        for v, f in a.funcs.get(seg.name, {}).items():
            x = a.key(seg, v, ovl)   # an overlay's function is keyed with its ROM offset in live and nm (Mario Party 3)
            if x not in live or nm[x] in replaced:
                continue
            n = (f.end - f.start) // 4
            off = seg.rom + (f.start - seg.vram)
            ws = words(rom, off, n)
            why, want = None, None
            his, rets = {}, set()   # HOOK_JRET: registers a lui set, and those holding an address in the function
            for i, w in enumerate(ws):
                ins = Ins(w, f.start + 4 * i)
                if hooked and HOOK_JRET:
                    if ins.is_j and rets and not f.start <= ins.target < f.end:
                        why = f'j {ins.target:08X}: a call that returns through a register (a hook)'
                        break
                    if ins.op == 15:   # lui
                        his[ins.rt] = ins.imm << 16
                    elif ins.op in (9, 13) and ins.rs in his and ins.rs == ins.rt:   # addiu, ori
                        v = (his.pop(ins.rs) + ins.simm if ins.op == 9 else his.pop(ins.rs) | ins.imm) & 0xFFFFFFFF
                        if ins.pc < v < max(f.end, ins.pc + 0x40):   # the analysis may end the function at the j
                            rets.add(ins.rt)
                # a jal into the hack's own code where no function starts: its code may jump back into the caller instead of
                # returning (WWF No Mercy Plus V3's standing special, 0x804921E0, from 0x8012A2BC: `j 0x8012A470`); called through
                # the lookup, the safety net ran the rest of the caller and then the caller again, and the interpreter met the
                # word at 0xC4 (the console). The interpreter runs the caller and follows it
                # A function found there (the analysis's, or learnt from the console) may still jump back: the shop's call
                # of 0x80471664, learnt as a function, did, and the interpreter met the word at 0x69 (the console)
                hseg = next((s for s in hook_segs if s.has(ins.target)), None) if hooked and ins.is_jal else None
                if hseg is not None and (ins.target not in starts or _hook_jumps_back(rom, hseg, ins.target, starts, jumps_back)):
                    why = f'jal {ins.target:08X}: into the hack\'s code, which jumps back into its caller (a hook)'
                    break
                if (OVL_LOOKUP and LOOKUP_JAL and lookups is not None and ins.is_jal and ins.target in starts and
                        ins.target not in a.funcs.get(seg.name, {}) and
                        any(s.name in ovl and s.has(ins.target) for s in a.img.segments)):
                    lookups.add(ins.target)
                    continue
                if ins.is_jal and not seg.has(ins.target) and ins.target not in starts:
                    if LOOKUP_JAL and lookups is not None:
                        lookups.add(ins.target)   # the runtime finds it, or its safety net runs it and learns it
                        continue
                    why = f'jal {ins.target:08X}: no function there'
                    t = ins.target
                    if (KUSEG_WANT and seg.name.startswith('code_') and (t < 0x80000000) == (f.start < 0x80000000)
                            and (0x10000 <= t < 0x80000000 or 0x80000400 <= t < 0x80800000) and abs(t - f.start) <= 0x100000):
                        # captured code calling code no capture holds yet (decompressed with it, never run so far):
                        # the callee's page, for the next round to find the function there
                        want = (t & ~0xFFF, (t & ~0xFFF) + 0x1000)
                    break
                if ins.is_jal and seg.name.startswith('code_') and seg.has(ins.target) and ins.target not in starts:
                    # into captured code where no function starts: another overlay's function at that address
                    # (N64Recomp would make a static there and split the function that holds it)
                    if LOOKUP_JAL and lookups is not None:
                        lookups.add(ins.target)   # whichever overlay is there when it runs (ovl.c, guard.c)
                        continue
                    why = f'jal {ins.target:08X}: another overlay\'s function'
                    break
                if ins.op == 17 and ins.rs in (16, 17) and 8 <= ins.funct <= 11:   # round/trunc/ceil/floor.l: not in N64Recomp
                    why = f'{("round", "trunc", "ceil", "floor")[ins.funct - 8]}.l.{"s" if ins.rs == 16 else "d"}'
                    break
                if ins.op in (48, 52, 56, 60):   # ll, lld, sc, scd: not in N64Recomp, which stops the whole run without
                    # naming the function ("Unhandled instruction: scd": Ken Griffey's two with the dry boot's code)
                    why = ('ll', 'lld', 'sc', 'scd')[(ins.op - 48) // 4]
                    break
                if ins.op == 16 and ins.rs in (0, 4) and ins.rd != 12:   # mfc0/mtc0: N64Recomp takes Status only
                    why = f'{"mfc0" if ins.rs == 0 else "mtc0"} of cop0 register {ins.rd}'
                    break
                if hooked and ins.is_j and seg.has(ins.target) and not f.start <= ins.target < f.end and ins.target not in starts:
                    why = f'j {ins.target:08X}: no function there (a hook)'
                    break
                # a `j` from the game's code into the hack's copied code is a hook too, even where a function was found there:
                # the shop's 0x801465C4 jumps to 0x80454700, which jumps back to 0x801465C8; recompiled as a tail call, the
                # interpreter resumed the shop's function on the wrong stack (sp 0x38 off, a return to 0x69 on the console)
                if (hooked and ins.is_j and seg not in hook_segs and
                        any(s.has(ins.target) for s in hook_segs)):
                    why = f'j {ins.target:08X}: into the hack\'s code (a hook)'
                    break
                if (ins.is_branch and not ins.is_link_branch) or ins.is_j:
                    t = ins.target if ins.is_j else ins.branch_target
                    if not seg.has(t) and t not in starts:   # past the segment: a label C cannot have
                        why = f'branch to {t:08X}, past its segment'
                        if seg.name.startswith('code_') and 0x80000000 <= f.start < t < 0x80800000 and t - f.start <= 0x100000:
                            want = (f.start, t + 4)   # the rest of the function, in RDRAM no capture holds
                        elif (KUSEG_WANT and seg.name.startswith('code_') and 0x10000 <= f.start < 0x80000000
                              and 0x10000 <= t < 0x80000000 and abs(t - f.start) <= 0x4000
                              and not any(s.has(t) for s in a.img.segments)):
                            # the same at KUSEG (Perfect Dark's TLB-mapped code, a page the run never ran), when
                            # no segment holds it and it is near: a far one is a walk gone wrong, whose pages
                            # beside GoldenEye's 0x70200000 cost that code its match with the ROM
                            want = (min(f.start, t), max(f.end, t + 4))
                        break
            if why is None:
                for t in _n64recomp_tables(ws, f.start):
                    r = (t - f.start + off) & 0xFFFFFFFF
                    if not seg.has(t) and _table_elsewhere(a, rom, t, f):
                        continue   # patches/recomp/0001: read from the segment that holds it
                    if not seg.has(t) or r + 4 > len(rom):
                        why = f'jump table at {t:08X}, outside its segment'
                        lo, hi = min(f.start, t), max(f.end, t + 4)
                        if seg.name.startswith('code_') and 0x80000000 <= lo and hi <= 0x80800000 and hi - lo <= 0x100000:
                            want = (lo, hi)
                        elif f.start < 0x80000000 and 0x80000000 <= t < 0x80800000:
                            # TLB-mapped code, its table in KSEG0: that page and the next. A table's length is not known here, and the
                            # recompiler stops at the end of what it can read: South Park's printf table crossed a page, its
                            # 'd' fell past the switch ("Jump table out of range")
                            want = (t & ~0xFFF, (t & ~0xFFF) + 0x2000)
                        elif (KUSEG_WANT and seg.name.startswith('code_') and 0x10000 <= t < 0x80000000
                              and f.start < 0x80000000 and abs(t - f.start) <= 0x100000):
                            # code captured at KUSEG with its table in KUSEG too (All-Star Baseball 2000's game,
                            # decompressed to 0x00100000, its rodata pages never run): the table's page and the
                            # next, a segment of their own the patched recompiler reads it from
                            want = (t & ~0xFFF, (t & ~0xFFF) + 0x2000)
                        break
            if why:
                out[x] = (nm[x], why, want)
    return out
