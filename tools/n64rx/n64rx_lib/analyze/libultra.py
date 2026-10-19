"""libultra by behaviour (PLAN.md 3.14, step 3): the functions the runtime
replaces, named from what they do, with no fingerprint of any ROM.

Anchors first, things only libultra does: the COP0 interrupt mask
(__osDisableInt / __osRestoreInt), Count and Compare, the cache loops, the
TLB probe, and the hardware registers (VI, AI, PI, SI, SP, DP, MI). Then the
names spread along the call graph and the struct offsets libultra's types
fix (OSMesgQueue: validCount at 8, first at 12, msgCount at 16; OSThread:
priority at 4, state at 16; OSIoMesg; the VI context behind __osViNext).
Rules run until nothing changes; every name keeps the rule that gave it, for
the report. A rule names a function only when exactly one candidate fits.

Version notes: rules lean on what is stable from libultra 2.0D to 2.0L
(field offsets, which registers a function writes, which helpers it calls);
where versions differ (osPiStartDma through osEPiStartDma, __osInitialize_
common behind osInitialize) the rule covers both shapes."""
import os

from .features import features
from .mips import Ins

# kill switch: N64RX_MESG_NODIV=0 keeps the queue rule to IDO's divide
MESG_NODIV = os.environ.get('N64RX_MESG_NODIV', '1') != '0'
# kill switch: N64RX_ULTRA_TWINS=0 names only the first of two copies a rule fits (Mario Tennis: osStartThread)
TWINS = os.environ.get('N64RX_ULTRA_TWINS', '1') != '0'
# kill switch: N64RX_KSEG_PROBE=0 names a tlbp function with the KSEG0 mask __osProbeTLB, as before
KSEG_PROBE = os.environ.get('N64RX_KSEG_PROBE', '1') != '0'

AI, PI, SI, SP, VI, MI, DP = 0x04500000, 0x04600000, 0x04800000, 0x04040000, 0x04400000, 0x04300000, 0x04100000


class Namer:
    def __init__(self, img, funcs):
        self.img = img
        self.funcs = funcs
        self.f = {a: features(img, fn) for a, fn in funcs.items()}
        self.name = {}      # addr -> name
        self.addr = {}      # name -> addr
        self.why = {}       # addr -> rule
        self.twins = {}     # addr -> name: a second copy of a named function (romonly.py: <name>__2)
        self.callers = {}
        for a, ft in self.f.items():
            for c in ft.calls:
                self.callers.setdefault(c, set()).add(a)
        self.changed = False

    def set(self, a, name, why):
        if a in self.name or name in self.addr or a not in self.f:
            return
        self.name[a], self.addr[name], self.why[a] = name, a, why
        self.changed = True

    def one(self, cands, name, why):
        cands = [c for c in set(cands) if c not in self.name and c not in self.twins]
        if len(cands) == 1 and name not in self.addr:
            self.set(cands[0], name, why)

    def twin(self, a, name, why):
        """a fits the rule that named `name` at another address: the game's second copy of it"""
        if TWINS and a not in self.name and a not in self.twins and self.addr.get(name, a) != a:
            self.twins[a] = (name, why + ' (a second copy)')

    def has(self, name):
        return name in self.addr

    def a_(self, name):
        return self.addr.get(name)

    def called(self, a):
        """names of what a calls"""
        return [self.name.get(c, '?') for c in self.f[a].calls]

    def calls_all(self, a, *names):
        cs = set(self.called(a))
        return all(n in cs for n in names)

    def hw(self, a, base, off=None, rw=None):
        for k, v in self.f[a].hw.items():
            if (k & 0xFFFF0000) == base and (off is None or k == base + off) and (rw is None or rw in v):
                return True
        return False

    def only_hw(self, a, *pairs):
        return set(self.f[a].hw) == {b + o for b, o in pairs}

    def arg_w(self, a, n, off):
        return any(k[0] == n and k[1] == off and 'w' in v for k, v in self.f[a].arg.items())

    def arg_r(self, a, n, off):
        return any(k[0] == n and k[1] == off and 'r' in v for k, v in self.f[a].arg.items())


def _kseg_mask(img, a, size):
    """arg0's low 29 bits returned (`sll t, a0, 3; srl v0, t, 3`, or an `and`
    with 0x1FFFFFFF): the KSEG0/KSEG1 path of osVirtualToPhysical"""
    shl = set()
    for pc in range(a, a + size, 4):
        w = img.word(pc)
        if w is None:
            break
        i = Ins(w, pc)
        if i.op == 0 and i.funct == 0 and i.rt == 4 and i.sa == 3 and i.rd:
            shl.add(i.rd)
        elif i.op == 0 and i.funct == 2 and i.rt in shl and i.sa == 3 and i.rd == 2:
            return True
        elif i.op == 15 and i.imm == 0x1FFF:   # lui 0x1FFF (then ori 0xFFFF and an and)
            return True
    return False


def _anchors(N):
    for a, ft in N.f.items():
        c0, leaf, sz = ft.cop0, ft.leaf, ft.size
        # osGetMemSize: writes 0x12345678 and 0x87654321 through uncached RDRAM a MB at a time
        if {0x12340000, 0x87650000, 0xA0000000} <= ft.consts and not c0 and sz <= 0x200:
            N.one([a], 'osGetMemSize', 'the 0x12345678 / 0x87654321 probe through KSEG1 RDRAM')
        if leaf and c0 == {('mf', 12), ('mt', 12)} and not ft.hw:
            if ft.returns_value and 0 not in ft.args_used:
                N.one([a], '__osDisableInt', 'mfc0/mtc0 Status, clears IE, returns the old IE')
            elif 0 in ft.args_used and not ft.returns_value:
                N.one([a], '__osRestoreInt', 'mfc0/mtc0 Status, ors in its argument')
        if c0 == {('mf', 12), ('mt', 12)} and N.hw(a, MI, 0xC):
            N.one([a], 'osSetIntMask', 'Status and MI_INTR_MASK')
        if leaf and c0 == {('mf', 9)} and sz <= 0x10:
            N.one([a], 'osGetCount', 'mfc0 Count, returns it')
        if leaf and c0 == {('mt', 9)} and sz <= 0x10:
            N.one([a], 'osSetCount', 'mtc0 Count')
        if leaf and c0 == {('mt', 11)} and sz <= 0x10:
            N.one([a], '__osSetCompare', 'mtc0 Compare')
        if leaf and c0 == {('mf', 12)} and sz <= 0x10 and not ft.hw:
            N.one([a], '__osGetSR', 'mfc0 Status only')
        if leaf and c0 == {('mt', 12)} and sz <= 0x14:
            N.one([a], '__osSetSR', 'mtc0 Status only')
        if leaf and c0 == {('mf', 13)} and sz <= 0x10:
            N.one([a], '__osGetCause', 'mfc0 Cause only')
        if leaf and not c0 and ft.fcr == {('cf', 31), ('ct', 31)} and sz <= 0x14:
            N.one([a], '__osSetFpcCsr', 'cfc1/ctc1 $31, returns the old one')
        if leaf and ft.cache:
            if ft.cache == {1} and not ft.args_used:
                N.one([a], 'osWritebackDCacheAll', 'index writeback-invalidate over the whole D-cache')
            elif ft.cache == {1, 25}:
                N.one([a], 'osWritebackDCache', 'hit writeback over a range (index over all when large)')
            elif ft.cache == {5, 25} and not ft.args_used:   # Camelot's: each line's tag loaded, then hit writeback
                N.one([a], 'osWritebackDCacheAll', 'index load tag and hit writeback over the whole D-cache')
            elif ft.cache == {5, 25} or ft.cache == {25} and ft.args_used == {0, 1}:
                N.one([a], 'osWritebackDCache', 'hit writeback over a range (tag loads over all when large)')
            elif 17 in ft.cache or 21 in ft.cache:
                N.one([a], 'osInvalDCache', 'hit invalidate D-cache over a range')
            elif 16 in ft.cache:
                N.one([a], 'osInvalICache', 'hit invalidate I-cache over a range')
        if 'tlbp' in ft.special and 'tlbwi' not in ft.special:
            if KSEG_PROBE and _kseg_mask(N.img, a, sz):   # Camelot's: osVirtualToPhysical with the probe inside
                N.one([a], 'osVirtualToPhysical', 'masks KSEG0/KSEG1 addresses to 29 bits, tlbp for the rest')
            else:
                N.one([a], '__osProbeTLB', 'tlbp')
        # the TLB writers: osMapTLB(index, pm, vaddr, even, odd, asid) takes
        # everything from its arguments, osUnmapTLB(index) only the index,
        # osUnmapTLBAll loops over every entry, osMapTLBRdb maps a fixed one
        if 'tlbwi' in ft.special and 'tlbp' not in ft.special and not ft.calls:
            if ft.args_used >= {0, 1, 2, 3}:
                N.one([a], 'osMapTLB', 'tlbwi of an entry made from its arguments')
            elif ft.args_used == {0}:
                N.one([a], 'osUnmapTLB', 'tlbwi of an invalid entry at the index in arg0')
            elif not ft.args_used and ft.branches:
                N.one([a], 'osUnmapTLBAll', 'tlbwi of an invalid entry at every index, in a loop')
            elif not ft.args_used:
                N.one([a], 'osMapTLBRdb', 'tlbwi of a fixed entry, no arguments')
    _ll_helpers(N)


# libultra's ll.c, compiled by IDO: (a0:a1, a2:a3) joined with dsll32, one
# 64-bit operation, the result split back into v0:v1. __ll_rem and
# __ull_rem compile to the same ddivu; the runtime gives both the same
# meaning, so either name is right for either copy.
LL = {('ddivu', 'lo'): ['__ull_div'], ('ddivu', 'hi'): ['__ull_rem', '__ll_rem'], ('ddiv', 'lo'): ['__ll_div'],
      ('dmultu', 'lo'): ['__ll_mul'], ('dmult', 'lo'): ['__ll_mul']}


def _ll_helpers(N):
    for a, ft in sorted(N.f.items()):
        if a in N.name or not ft.leaf or ft.size > 0x80 or ft.calls or ft.globals_:
            continue
        if len(ft.divs) == 1:
            kind, part = next(iter(ft.divs))
            if kind == 'ddiv' and part == 'hi' and ft.branches:
                N.one([a], '__ll_mod', 'ddiv, the remainder made to follow the sign of the divisor')
                continue
            for name in LL.get((kind, part), []):
                if not N.has(name):
                    N.one([a], name, f'one {kind}, the {part} half returned as a register pair')
                    break
        elif not ft.divs and len(ft.shifts64) == 1 and ft.size <= 0x40:
            name = {'dsllv': '__ll_lshift', 'dsrlv': '__ull_rshift', 'dsrav': '__ll_rshift'}[next(iter(ft.shifts64))]
            N.one([a], name, f'one {next(iter(ft.shifts64))} on a register pair')
        elif not ft.divs and ft.cvt_l and ft.size <= 0x80:
            kind = next(iter(ft.cvt_l))
            name = ('__ull_to_' if ft.branches else '__ll_to_') + ('d' if kind == 'd' else 'f')
            N.one([a], name, f'cvt.{kind}.l of a register pair' + (', with the unsigned correction' if ft.branches else ''))


def _hardware(N):
    # every function is tried first; a name is given only when one fits
    cands = {}

    def one(c, name, why):
        cands.setdefault(name, [set(), why])[0].update(c)
    for a, ft in N.f.items():
        sz = ft.size
        if ft.leaf and N.only_hw(a, (AI, 4)) and sz <= 0x14:
            one([a], 'osAiGetLength', 'reads AI_LEN')
        if ft.leaf and N.only_hw(a, (AI, 0xC)) and sz <= 0x10:
            one([a], 'osAiGetStatus', 'reads AI_STATUS')
        # (__osInitialize_common of 2.0I and later writes the two rates too, among other blocks)
        if N.hw(a, AI, 0x10, 'w') and N.hw(a, AI, 0x14, 'w') and all(k & 0xFFFF0000 == AI for k in ft.hw):
            one([a], 'osAiSetFrequency', 'writes AI_DACRATE and AI_BITRATE')
        if N.hw(a, AI, 0, 'w') and N.hw(a, AI, 4, 'w'):
            one([a], 'osAiSetNextBuffer', 'writes AI_DRAM_ADDR and AI_LEN')
        if N.hw(a, DP, 0, 'w') and N.hw(a, DP, 4, 'w'):
            one([a], 'osDpSetNextBuffer', 'writes DPC_START and DPC_END')
        if N.hw(a, SP, 0x10) and N.hw(a, 0x04080000, 0, 'w'):
            one([a], '__osSpSetPc', 'writes SP_PC')
        if ft.leaf and N.only_hw(a, (SP, 0x10)) and 'w' in ft.hw.get(SP + 0x10, '') and sz <= 0x14:
            one([a], '__osSpSetStatus', 'writes SP_STATUS only')
        if ft.leaf and N.only_hw(a, (SP, 0x10)) and ft.hw.get(SP + 0x10) == 'r':
            one([a] if sz <= 0x10 else [], '__osSpGetStatus', 'reads SP_STATUS only')
        if N.hw(a, SP, 0, 'w') and N.hw(a, SP, 4, 'w') and N.hw(a, SP, 8, 'w'):
            one([a], '__osSpRawStartDma', 'writes SP_MEM_ADDR, SP_DRAM_ADDR, SP_RD/WR_LEN')
        if not ft.args_used and ft.returns_value and not N.hw(a, PI, 0, 'w') and \
                all(N.hw(a, PI, o) for o in (0x14, 0x18, 0x1C, 0x20)) and 0xB0000000 in ft.consts:
            # the cartridge's handle: domain 1's timing registers, the ROM's KSEG1 base, no DMA
            one([a], 'osCartRomInit', 'PI domain 1 timing registers and the cartridge base, returns the handle')
        if ft.leaf and ft.args_used == {0, 1, 2} and N.hw(a, PI, 0x10, 'r') and not N.hw(a, PI, 0, 'w') and \
                N.arg_r(a, 0, 12) and all(N.hw(a, PI, o, 'w') for o in (0x14, 0x18, 0x1C, 0x20)):
            # (handle, devAddr, data or *data): the handle's domain timings, PI_STATUS polled, one word
            # through the handle's KSEG1 base (+12)
            one([a], '__osEPiRawReadIo' if any(k[0] == 2 and 'w' in v for k, v in ft.arg.items())
                else '__osEPiRawWriteIo', 'sets the handle\'s PI domain timings, polls PI_STATUS, one word')
        if N.hw(a, PI, 0, 'w') and N.hw(a, PI, 4, 'w') and (N.hw(a, PI, 8, 'w') or N.hw(a, PI, 0xC, 'w')):
            # the first argument is a direction in osPiRawStartDma, a handle in osEPiRawStartDma
            if any(k[0] == 0 for k in ft.arg):
                one([a], 'osEPiRawStartDma', 'PI DMA registers, the handle\'s fields read')
            else:
                one([a], 'osPiRawStartDma', 'PI DMA registers')
        if N.hw(a, SI, 0, 'w') and N.hw(a, SI, 0x10, 'w') or N.hw(a, SI, 0, 'w') and N.hw(a, SI, 4, 'w'):
            one([a], '__osSiRawStartDma', 'SI DMA registers')
        elif N.hw(a, SI, 0, 'w') and N.hw(a, SI, 0x18) and not ft.leaf:
            # an older one: the busy check inline, the PIF address register picked through a branch the
            # feature walk does not follow (Mario Party)
            one([a], '__osSiRawStartDma', 'SI_DRAM_ADDR written after an inline SI_STATUS check')
        if ft.leaf and N.only_hw(a, (SI, 0x18)):
            one([a], '__osSiDeviceBusy', 'reads SI_STATUS')
        if N.hw(a, VI) and sum(1 for k in ft.hw if k & 0xFFFF0000 == VI and 'w' in ft.hw[k]) >= 8:
            one([a], '__osViSwapContext', 'writes most VI registers')
        # two arguments: Acclaim's loaders have a copy loop of the same shape with a third, the size
        # (All-Star Baseball 2000: named this, it read one word of the game's code; N64RX_PIREAD_ARGS=0)
        if ft.leaf and N.only_hw(a, (PI, 0x10)) and N.arg_w(a, 1, 0) and \
                (2 not in ft.args_used or os.environ.get('N64RX_PIREAD_ARGS', '1') == '0'):
            one([a], 'osPiRawReadIo', 'polls PI_STATUS, stores the word to *arg1')
    for name, (c, why) in cands.items():
        N.one(sorted(c), name, why)


def _structure(N):
    f = N.f
    di, ri = N.a_('__osDisableInt'), N.a_('__osRestoreInt')
    for a, ft in f.items():
        if a in N.name:
            continue
        # OSMesgQueue init: six stores at arg0 +0..+20, nothing else
        if ft.leaf and ft.args_used >= {0, 1, 2} and all(N.arg_w(a, 0, o) for o in (0, 4, 8, 12, 16, 20)) and \
                not ft.hw and not ft.cop0 and ft.size <= 0x40:
            N.one([a], 'osCreateMesgQueue', 'stores the six fields of an OSMesgQueue at arg0')
        # IDO's long helpers that never divide: shifts and multiplies on register pairs (64-bit, leaf, tiny)
    if di and ri:
        mesg = [a for a, ft in f.items() if di in ft.calls and ri in ft.calls and 'div' in ft.special
                and N.arg_r(a, 0, 8) and N.arg_r(a, 0, 16)]
        if MESG_NODIV:
            # a libultra built another way (Mario Tennis: its osJamMesg divides, its
            # osSendMesg and osRecvMesg do not): the ring index wraps by a compare,
            # __osRestoreInt is a tail jump; validCount written, the ring read
            mesg += [a for a, ft in f.items() if a not in mesg and 'div' not in ft.special and di in ft.calls and
                     (ri in ft.calls or ri in ft.tails) and N.arg_r(a, 0, 8) and N.arg_w(a, 0, 8) and
                     N.arg_r(a, 0, 16) and N.arg_r(a, 0, 20) and ft.size <= 0x200]
        for a in mesg:
            if N.arg_w(a, 1, 0) or any(k[0] == 1 and 'w' in v for k, v in f[a].arg.items()):
                N.one([a], 'osRecvMesg', 'queue at arg0: validCount, msgCount, first; stores the message at *arg1')
            elif 'div' not in f[a].special and N.arg_w(a, 0, 12) and N.arg_r(a, 0, 4) and not N.arg_r(a, 0, 0):
                # arg1 spilled and reloaded (Mario Tennis): advances `first`, wakes the fullqueue (+4)
                N.one([a], 'osRecvMesg', 'queue at arg0: takes from `first`, wakes a sender (no divide)')
            elif N.arg_w(a, 0, 12):
                N.one([a], 'osJamMesg', 'queue at arg0: moves `first` back and puts the message there')
            else:
                N.one([a], 'osSendMesg', 'queue at arg0: validCount < msgCount, appends')
        for a, ft in f.items():
            if a in N.name or di not in ft.calls or ri not in ft.calls:
                continue
            # OSThread: arg0 + 16 is the state (u16), + 20 the id, a big register context
            if N.arg_w(a, 0, 16) and N.arg_w(a, 0, 20) and any(k[0] == 0 and k[1] >= 0x100 for k in ft.arg):
                N.one([a], 'osCreateThread', 'fills an OSThread at arg0: state, id, the context')
    # the thread helpers the queue functions share
    for nm in ('osSendMesg', 'osRecvMesg', 'osJamMesg'):
        a = N.a_(nm)
        if not a:
            continue
        for c in f[a].calls:
            fc = f.get(c)
            if fc and c not in N.name and ('64' in fc.special or fc.fcr) and ('mf', 12) in fc.cop0:
                N.one([c], '__osEnqueueAndYield', 'saves the running thread\'s context (Status, sd, FPU csr)')
    eay = N.a_('__osEnqueueAndYield')
    st = N.a_('osStartThread')
    for a, ft in f.items():
        if a in N.name or not di or di not in ft.calls:
            continue
        if 'eret' in ft.special:
            continue
        if ft.args_used == {0} and N.arg_r(a, 0, 8) and N.arg_w(a, 0, 16) and eay and eay in ft.calls and \
                sum(1 for c in ft.calls if c not in (di, ri, eay)) >= 2:
            N.one([a], 'osStartThread', 'reads the thread\'s queue (+8), sets its state (+16), dispatches')
            # Mario Tennis links two: one with the `__osRunningThread == NULL` dispatch, one without
            N.twin(a, 'osStartThread', 'reads the thread\'s queue (+8), sets its state (+16), dispatches')
    st = N.a_('osStartThread')
    # osDestroyThread(t): dequeues it and, when it is the running one, dispatches the next (the eret)
    disp = [a for a, ft in f.items() if 'eret' in ft.special and a not in N.name]
    if di and disp:
        cands = [a for a, ft in f.items() if a not in N.name and di in ft.calls and ft.args_used == {0} and
                 any(d in ft.calls for d in disp) and not (eay and eay in ft.calls)]
        N.one(cands, 'osDestroyThread', 'dequeues the thread and dispatches the next one')
    # osStopThread(t): reads the state (+16), dequeues, yields when it stops itself
    if di and eay:
        cands = [a for a, ft in f.items() if a not in N.name and di in ft.calls and eay in ft.calls and
                 ft.args_used == {0} and N.arg_r(a, 0, 16) and N.arg_w(a, 0, 16) and a != st and
                 (st or not N.arg_r(a, 0, 8))]   # ultralib's dequeues from t->queue (+8) too
        N.one(cands, 'osStopThread', 'reads the thread state (+16), stops it, yields when it is the caller')
    # osSetThreadPri: dequeue + enqueue + yield, two args, writes priority via a pointer
    if eay:
        cands = [a for a, ft in f.items() if a not in N.name and di in ft.calls and eay in ft.calls and
                 ft.args_used >= {0, 1} and not ft.special and len(set(ft.calls)) >= 4 and a != st and
                 not N.arg_r(a, 0, 8) and not N.arg_r(a, 0, 16)]
        N.one(cands, 'osSetThreadPri', 'dequeues and re-enqueues the thread with a new priority, then yields')
    # osGetThreadPri(t): a leaf that returns t->priority (or the running thread's)
    cands = [a for a, ft in f.items() if a not in N.name and ft.leaf and ft.size <= 0x20 and ft.args_used == {0}
             and ft.returns_value and (N.arg_r(a, 0, 4) or len(ft.globals_) == 1)]
    users = set()
    for m in ('osCreatePiManager', 'osCreateViManager'):
        pass
    if cands:
        # among leaf getters, the one osSetThreadPri's callers pair it with
        stp = N.a_('osSetThreadPri')
        if stp:
            pairs = [c for c in cands if any(stp in f[x].calls and c in f[x].calls for x in N.callers.get(c, ()))]
            N.one(pairs, 'osGetThreadPri', 'returns arg0->priority; called next to osSetThreadPri')
    # the thread getters by their shape: t, or the running thread when t is NULL
    # (__osRunningThread, a global the runtime never sets: a game's own copy reads
    # NULL+0x14), then one field: +0x14 id (osGetThreadId), +0x4 priority
    # (osGetThreadPri). Both read the same global; a name only when one fits each.
    getters = {}
    for a, ft in f.items():
        if a in N.name or not ft.leaf or ft.size > 0x20 or ft.args_used != {0} or not ft.returns_value:
            continue
        if len(ft.globals_) != 1 or len(ft.gptr) != 1:
            continue
        (g, off, size), = ft.gptr.keys()
        if size == 4 and off in (0x14, 0x4) and list(ft.globals_)[0] == g:
            getters.setdefault(off, []).append((a, g))
    for off, name in ((0x14, 'osGetThreadId'), (0x4, 'osGetThreadPri')):
        cs = getters.get(off, [])
        other = getters.get(0x4 if off == 0x14 else 0x14, [])
        if len(cs) == 1 and (not other or len(other) != 1 or other[0][1] == cs[0][1]):
            N.one([cs[0][0]], name, f'returns arg0 (or the running thread) +{off:#x}')
    # managers: create a queue, a thread, start it, set an event
    ct = N.a_('osCreateThread')
    cq = N.a_('osCreateMesgQueue')
    if ct and st:
        mgrs = [a for a, ft in f.items() if a not in N.name and ct in ft.calls and st in ft.calls]
        sw = N.a_('__osViSwapContext')
        pim = []
        for a in mgrs:
            callees = set(f[a].calls)
            vi = any(sw and sw in f[c].calls for c in callees if c in f)
            # (osCreateScheduler starts a thread too, but also another manager: a callee that starts one)
            starts_mgr = any(f.get(c) and ct in f[c].calls for c in callees if c not in (ct, st))
            if vi:
                N.one([a], 'osCreateViManager', 'starts the VI thread and calls the VI init (writes the VI registers)')
            elif (cq and list(f[a].calls).count(cq) >= 2 or any(f.get(c) and cq in f[c].calls for c in callees
                                                                if c != cq)) and not starts_mgr:
                pim += [a] if len(f[a].args_used) >= 3 else []
        sem = N.a_('osSetEventMesg')
        if len(pim) > 1 and sem:   # the real one registers its event queue for OS_EVENT_PI
            pim = [a for a in pim if sem in f[a].calls]
        if len(pim) > 1 and sem:   # with OS_EVENT_PI (8) as the event: a game's own scheduler registers SP/DP
            from .segments import _consts
            pi8 = [a for a in pim if any(t == sem and args.get(4) == 8 for _pc, t, args in _consts(N.img, N.funcs[a]))]
            pim = pi8 or pim
        N.one(pim, 'osCreatePiManager', 'starts the PI device manager thread with its queues')
        for a in [m for m in mgrs if m not in N.name]:
            pass
    # osSetEventMesg(event, mq, msg): disable, two stores into a table indexed by arg0, restore;
    # the later libultras also send the message at once for OS_EVENT_PRENMI when a
    # reset is already pending (osSendMesg, a little longer: Ridge Racer 64)
    if di and ri:
        sm = N.a_('osSendMesg')
        ok = {di, ri} | ({sm} if sm else set())
        cands = [a for a, ft in f.items() if a not in N.name and {di, ri} <= set(ft.calls) <= ok and
                 ft.args_used >= {0, 1, 2} and ft.size <= 0xC0 and not ft.gptr]
        vim = N.a_('osCreateViManager')
        if vim:
            cands = [c for c in cands if c in f[vim].calls]
        N.one(cands, 'osSetEventMesg', 'stores (queue, message) in the event table at arg0, interrupts off')


def _vi(N):
    """The VI calls all go through the context __osViNext points at (struct
    __OSViContext: state +0 u16, retraceCount +2, framep +4, modep +8,
    control +12, msgq +16, msg +20, x.factor +24, y.factor +36)."""
    f = N.f
    sw = N.a_('__osViSwapContext')
    if not sw:
        return
    ctx = [g for g, v in f[sw].globals_.items() if 'r' in v and 'w' in v]
    di, ri = N.a_('__osDisableInt'), N.a_('__osRestoreInt')
    if len(ctx) < 2 or not di:
        return
    # __osViCurr and __osViNext: next is the one osViSwapBuffer & co write through
    users = [a for a, ft in f.items() if a not in N.name and di in ft.calls and ri in ft.calls and
             any(g in ft.globals_ for g in ctx)]
    for a in users:
        ft = f[a]
        stores = {(g, o) for (g, o, s), v in ft.gptr.items() if 'w' in v and g in ctx}
        loads_g = {g for g in ctx if g in ft.globals_}
        src = ft.gptr_src
        offs = {o for _g, o in stores}
        if not ft.args_used and ft.returns_value and not stores:
            # the current or the next framebuffer: which global decides, settled below
            continue
        if 8 in offs and 12 in offs:
            N.one([a], 'osViSetMode', 'stores arg0 as the next context\'s mode (+8) and its control word (+12)')
        elif 16 in offs and 20 in offs:
            N.one([a], 'osViSetEvent', 'stores (queue, message, retrace count) in the next context')
        elif 4 in offs and ft.args_used == {0}:
            N.one([a], 'osViSwapBuffer', 'stores arg0 as the next context\'s framebuffer (+4)')
        elif 24 in offs and ft.float_args:
            N.one([a], 'osViSetXScale', 'stores a float as the x scale (+24)')
        elif 36 in offs and ft.float_args:
            N.one([a], 'osViSetYScale', 'stores a float as the y scale (+36)')
        elif 12 in offs and ft.args_used == {0} and ft.size > 0x100:
            N.one([a], 'osViSetSpecialFeatures', 'rewrites the next context\'s control word (+12) bit by bit')
        elif offs == {0} and ft.args_used == {0}:
            N.one([a], 'osViBlack', 'sets or clears the blank bit in the next context\'s state (+0)')
    # the framebuffer getters: return context->framep; the one reading the global osViSwapBuffer writes through is "next"
    swb = N.a_('osViSwapBuffer')
    nxt = None
    if swb:
        g = [g for (g, o, s), v in f[swb].gptr.items() if o == 4 and 'w' in v]
        nxt = g[0] if g else None
    getters = [a for a, ft in f.items() if a not in N.name and di in ft.calls and ri in ft.calls and
               not ft.args_used and ft.returns_value and len([g for g in ctx if g in ft.globals_]) == 1 and
               any(o == 4 for (_g, o, _s) in ft.gptr)]
    for a in getters:
        g = [g for g in ctx if g in f[a].globals_][0]
        if nxt is not None:
            N.one([a], 'osViGetNextFramebuffer' if g == nxt else 'osViGetCurrentFramebuffer',
                  'returns the ' + ('next' if g == nxt else 'current') + ' context\'s framebuffer (+4)')
    # osViGetCurrentMode: the current context's mode (+8), its type byte; the game's own __osViCurr stays
    # NULL under the runtime's VI (Donkey Kong 64 read NULL+3)
    modes = [a for a, ft in f.items() if a not in N.name and di in ft.calls and ri in ft.calls and
             not ft.args_used and ft.returns_value and len([g for g in ctx if g in ft.globals_]) == 1 and
             any(o == 8 for (_g, o, _s) in ft.gptr) and not any(o == 4 for (_g, o, _s) in ft.gptr)]
    if nxt is not None:
        cur = [a for a in modes if [g for g in ctx if g in f[a].globals_][0] != nxt]
        N.one(cur, 'osViGetCurrentMode', 'returns the current context\'s mode (+8) type')


def _io(N):
    f = N.f
    rs, ss = N.a_('osRecvMesg'), N.a_('osSendMesg')
    jm = N.a_('osJamMesg')
    # osPiStartDma / osEPiStartDma: fill an OSIoMesg (u16 type +0, u8 pri +2, retQueue +4, dram +8, dev +12,
    # size +16) and send it (osJamMesg for high priority)
    if ss and jm:
        for a, ft in f.items():
            if a in N.name or ss not in ft.calls or jm not in ft.calls:
                continue
            io = 1 if any(k[0] == 1 and k[1] in (8, 12, 16) for k in ft.arg) else 0
            if all(N.arg_w(a, io, o) for o in (4, 8, 12, 16)):
                N.one([a], 'osEPiStartDma' if io == 1 else 'osPiStartDma',
                      'fills the OSIoMesg at arg%d and sends or jams it to the PI manager' % io)
            elif ft.args_used >= {0, 1, 2} and N.arg_w(a, 1, 20) and N.arg_w(a, 1, 0) and \
                    not any(N.arg_w(a, 1, o) for o in (8, 12, 16)):
                # the caller filled the message: only its type and handle (+20) are set here (2.0L's)
                N.one([a], 'osEPiStartDma', 'sets the type and handle of the OSIoMesg at arg1, sends or jams it')
    # the SI and PI access queues: get = recv on a global queue, rel = send to it
    for kind in ('Si', 'Pi'):
        pass
    sid = N.a_('__osSiRawStartDma')
    if sid and rs and ss:
        users = [a for a, ft in f.items() if sid in ft.calls]
        # every SI transaction is wrapped in get / release access
        get = {}
        rel = {}
        for u in users:
            for c in f[u].calls:
                fc = f.get(c)
                if not fc or c in N.name:
                    continue
                if rs in fc.calls and len(fc.calls) <= 3 and not fc.args_used and fc.size <= 0x80:   # 0x6C: the queue made inline (Mario Party)
                    get[c] = get.get(c, 0) + 1
                if fc.calls == [ss] and not fc.args_used and fc.size <= 0x40:
                    rel[c] = rel.get(c, 0) + 1
        if get:
            N.one([max(get, key=get.get)], '__osSiGetAccess', 'receives from the SI access queue around SI DMAs')
        if rel:
            N.one([max(rel, key=rel.get)], '__osSiRelAccess', 'sends to the SI access queue after SI DMAs')
    sga, sra = N.a_('__osSiGetAccess'), N.a_('__osSiRelAccess')
    st_ = N.a_('osSetTimer')
    ctq = N.a_('osCreateMesgQueue')
    # osSetTimer(t, countdown, interval, mq, msg): fills an OSTimer at arg0 (+0..+28) and arms it
    for a, ft in f.items():
        if a in N.name:
            continue
        if sum(1 for o in (0, 4, 8, 12, 16, 20, 24, 28) if N.arg_w(a, 0, o)) >= 6 and \
                any(f.get(c) and N.a_('__osSetCompare') in f[c].calls for c in ft.calls
                    for _ in [0] if f.get(c)) or (sum(1 for o in (0, 4, 8, 12, 16, 20, 24, 28) if N.arg_w(a, 0, o)) >= 6
                                                  and any(f.get(c) and any(f.get(cc) and N.a_('__osSetCompare') in
                                                                           f[cc].calls for cc in f[c].calls)
                                                          for c in ft.calls)):
            N.one([a], 'osSetTimer', 'fills an OSTimer at arg0 and arms the Compare interrupt')
    st_ = N.a_('osSetTimer')
    gc, sc = N.a_('osGetCount'), N.a_('__osSetCompare')
    di, ri = N.a_('__osDisableInt'), N.a_('__osRestoreInt')
    # osStopTimer(t): unlinks the OSTimer at arg0 from the timer list with interrupts off, clears its
    # next and prev (+0, +4), and sets Compare to 0 when the list is empty. Unnamed, the game's own
    # copy unlinked a timer the runtime's osSetTimer never put in its list and wrote through stale
    # pointers (Ocarina of Time: its graph thread's RCP timeout timer; the game-state table corrupted)
    if sc and di and ri:
        cands = [a for a, ft in f.items() if a not in N.name and ft.args_used == {0} and
                 {di, ri, sc} <= set(ft.calls) and len(ft.calls) <= 4 and ft.size <= 0x100 and
                 N.arg_w(a, 0, 0) and N.arg_w(a, 0, 4)]
        N.one(cands, 'osStopTimer', 'unlinks the OSTimer at arg0 (clears +0 and +4) and resets Compare when none is left')
    if gc and di:
        cands = [a for a, ft in f.items() if a not in N.name and set(ft.calls) == {di, gc, ri} and
                 not ft.args_used and ft.returns_value and (not sc or sc not in ft.calls)]
        N.one(cands, 'osGetTime', 'Count with interrupts off, added to the time since the last read (64-bit)')
    # osContInit: SI DMAs and a timer wait, creating its queue. Named before the two below: the pack it
    # calls (the status request) tells osContStartQuery from osContStartReadData (Ocarina of Time's
    # came out swapped, and the game's own query then sent an empty PIF block: "NO CONTROLLER")
    if sid and st_:
        cands = [a for a, ft in f.items() if a not in N.name and sid in ft.calls and st_ in ft.calls and
                 ctq in ft.calls]
        N.one(cands, 'osContInit', 'waits with a timer, then SI DMAs to find the controllers')
    if sid and sga and sra:
        si_users = [a for a, ft in f.items() if a not in N.name and sga in ft.calls and sra in ft.calls]
        for a in si_users:
            ft = f[a]
            ncalls = len(ft.calls)
            if sid in ft.calls and rs in ft.calls and ft.args_used == {0}:
                # osContStartReadData / osContStartQuery: pack, two SI DMAs, wait
                packs = [c for c in ft.calls if c not in (sga, sra, sid, rs)]
                N.one([a] if packs else [], 'osContStartReadData' if not _packs_request(N, packs) else
                      'osContStartQuery', 'packs a controller command, two SI DMAs between the access calls')
            elif sid not in ft.calls and ncalls <= 4 and ft.args_used == {0}:
                N.one([a], 'osEepromProbe', 'asks the EEPROM its status between the SI access calls')
        for a in [x for x in si_users if x not in N.name]:
            ft = f[a]
            # (mq, address, buffer): __osContRamRead/Write take a channel too (arg 3 used)
            if sid in ft.calls and ft.args_used >= {0, 1, 2} and 3 not in ft.args_used:
                if any(k[0] == 2 and 'w' in v for k, v in ft.arg.items()):
                    N.one([a], 'osEepromRead', 'an EEPROM block read into *arg2')
                else:
                    N.one([a], 'osEepromWrite', 'an EEPROM block written from arg2')
    if sid and rs and not sga:
        # an older libultra with no SI access queue (Mario Party): osContStartReadData / osContStartQuery
        # pack, raw SI DMA, wait on the queue at arg0, raw SI DMA
        for a, ft in f.items():
            if a in N.name or sid not in ft.calls or rs not in ft.calls or ft.args_used != {0} or len(ft.calls) > 4:
                continue
            packs = [c for c in ft.calls if c not in (sid, rs)]
            if packs:
                N.one([a], 'osContStartReadData' if not _packs_request(N, packs) else 'osContStartQuery',
                      'packs a controller command, raw SI DMAs and a wait (no SI access queue)')
    er, ew = N.a_('osEepromRead'), N.a_('osEepromWrite')
    for a, ft in f.items():
        if a in N.name:
            continue
        if er and er in ft.calls and ft.args_used >= {0, 1, 2, 3}:
            N.one([a], 'osEepromLongRead', 'osEepromRead in a loop')
        if ew and ew in ft.calls and ft.args_used >= {0, 1, 2, 3}:
            N.one([a], 'osEepromLongWrite', 'osEepromWrite in a loop with a delay')
    # osContGetQuery(status): __osContGetInitData(&pattern, status), which osContInit calls too
    ci = N.a_('osContInit')
    if ci:
        cands = [a for a, ft in f.items() if a not in N.name and len(ft.calls) == 1 and ft.calls[0] in f[ci].calls
                 and ft.calls[0] not in N.name and ft.args_used == {0} and ft.size <= 0x40]
        N.one(cands, 'osContGetQuery', 'the status records at arg0, through the routine osContInit reads them with')
    # osContGetReadData(pads): a leaf copying the SI buffer into 6-byte OSContPad records at arg0
    # (the SI buffer can be named only by the routine osContStartReadData calls to pack it, so its
    # callees' globals count too; and an OSContPad has bytes at +2 and +3, so a halfword written at
    # arg0 + 2 is something else: Mario Party's own ring of recorded pads, 4 shorts a call, sat
    # within 0x100 of its last-command byte and took the name, its controls dead)
    srd = N.a_('osContStartReadData')
    if srd:
        pack = {g for c in f[srd].calls if c in f and c not in N.name for g in f[c].globals_}
        bufs = set(f[srd].globals_) | pack
        cands = [a for a, ft in f.items() if a not in N.name and ft.leaf and ft.args_used == {0} and
                 N.arg_w(a, 0, 0) and not any(k[:3] == (0, 2, 2) and 'w' in v for k, v in ft.arg.items()) and
                 any(g in bufs or abs(g - b) < 0x100 for g in ft.globals_ for b in bufs)]
        if len(cands) > 1 and pack:   # the one that reads the buffer the pack routine fills
            cands = [a for a in cands if set(f[a].globals_) & pack] or cands
        N.one(cands, 'osContGetReadData', 'copies the SI buffer into OSContPad records at arg0')
    # osPiReadIo: access, raw read, release
    rri = N.a_('osPiRawReadIo')
    if rri:
        cands = [a for a, ft in f.items() if a not in N.name and rri in ft.calls and len(ft.calls) == 3 and
                 ft.args_used >= {0, 1}]
        N.one(cands, 'osPiReadIo', 'osPiRawReadIo between the PI access calls')
    # osEPiReadIo / osEPiWriteIo: access, the handle's raw read or write, release
    for raw, name in (('__osEPiRawReadIo', 'osEPiReadIo'), ('__osEPiRawWriteIo', 'osEPiWriteIo')):
        r = N.a_(raw)
        if r:
            cands = [a for a, ft in f.items() if a not in N.name and r in ft.calls and len(ft.calls) == 3 and
                     ft.args_used == {0, 1, 2}]
            N.one(cands, name, f'{raw} between the PI access calls')


def _packs_request(N, packs):
    """the pack osContInit uses is the request (query) pack"""
    ci = N.a_('osContInit')
    return bool(ci and any(p in N.f[ci].calls for p in packs))


def _sp_init(N):
    f = N.f
    spc, sss, rsd = N.a_('__osSpSetPc'), N.a_('__osSpSetStatus'), N.a_('__osSpRawStartDma')
    wb = N.a_('osWritebackDCache')
    for a, ft in f.items():
        if a in N.name:
            continue
        if spc and rsd and spc in ft.calls and rsd in ft.calls and ft.args_used == {0}:
            N.one([a], 'osSpTaskLoad', 'loads the task: SP PC, the boot microcode and the task by SP DMA')
        if sss and set(ft.calls) == {sss} and not ft.args_used and ft.size <= 0x30:
            N.one([a], 'osSpTaskYield', 'sets SP_SET_YIELD')
    gs = N.a_('__osSpGetStatus')
    for a, ft in f.items():
        if a in N.name:
            continue
        if gs and gs in ft.calls and len(set(ft.calls)) == 1 and ft.returns_value:
            N.one([a], 'osSpTaskYielded', 'reads SP_STATUS: did the task yield')
        if sss and sss in ft.calls and not (rsd and rsd in ft.calls) and \
                not (spc and spc in ft.calls) and len(set(ft.calls)) == 2 and ft.size <= 0x60:
            N.one([a], 'osSpTaskStartGo', 'waits for the SP, then clears halt')
    # osInitialize: installs the exception vectors at 0x80000000/0x80000080/0x80000180
    for a, ft in f.items():
        if all(v in ft.globals_ for v in (0x80000000, 0x80000080, 0x80000180)):
            N.one([a], '__osInitialize_common' if any(N.name.get(c) == '__osInitialize_isv' for c in ft.calls) else
                  'osInitialize', 'writes the exception vectors at 0x80000000, 0x80000080, 0x80000180')
    oi = N.a_('osInitialize') or N.a_('__osInitialize_common')
    # osVirtualToPhysical: K0/K1 masks, the TLB probe otherwise
    pt = N.a_('__osProbeTLB')
    if pt:
        cands = [a for a, ft in f.items() if a not in N.name and set(ft.calls) == {pt} and ft.args_used == {0}]
        N.one(cands, 'osVirtualToPhysical', 'masks KSEG0/KSEG1 addresses, asks the TLB for the rest')


# ---- the TLB family and osYieldThread (added by the tlb work; the runtime's
# are tlb.c / ultra/more2.c / thread.c). Behaviour only: the TLB routines
# write Index, EntryHi, EntryLo0/1 and run tlbwi, told apart by their
# arguments and whether they set PageMask; osYieldThread takes none and is __osDisableInt,
# __osEnqueueAndYield, __osRestoreInt and nothing else.
TLB_WRITE = {('mt', 0), ('mt', 2), ('mt', 3), ('mt', 10)}


def _tlb(N):
    for a, ft in N.f.items():
        if a in N.name or 'tlbwi' not in ft.special or not TLB_WRITE <= ft.cop0:
            continue
        if {0, 1, 2, 3} <= ft.args_used and ('mt', 5) in ft.cop0:
            N.one([a], 'osMapTLB', 'PageMask, EntryHi, EntryLo0/1 from its arguments, tlbwi')
        elif ft.args_used == {0}:
            N.one([a], 'osUnmapTLB', 'tlbwi of an invalid entry at the index in arg0')
        elif not ft.args_used and ('mt', 5) in ft.cop0:
            N.one([a], 'osMapTLBRdb', 'PageMask and a fixed entry (the debugger port), no arguments')
        elif not ft.args_used:
            N.one([a], 'osUnmapTLBAll', 'tlbwi of invalid entries over the whole TLB, PageMask untouched')
    di, ri, eay = N.a_('__osDisableInt'), N.a_('__osRestoreInt'), N.a_('__osEnqueueAndYield')
    if di and ri and eay:
        cands = [a for a, ft in N.f.items() if a not in N.name and not ft.args_used and ft.calls == [di, eay, ri]]
        N.one(cands, 'osYieldThread', '__osDisableInt, __osEnqueueAndYield, __osRestoreInt, no arguments')


RULES = (_anchors, _hardware, _structure, _vi, _io, _sp_init, _tlb)


def identify(img, funcs):
    """{addr: (name, rule)} for the libultra functions found"""
    N = Namer(img, funcs)
    for _ in range(12):
        N.changed = False
        for r in RULES:
            r(N)
        if not N.changed:
            break
    return {a: (n, N.why[a]) for a, n in N.name.items()}, N
