"""What a function does, for the libultra rules (PLAN.md 3.14, step 3).

One pass over the function's instructions in address order with a simple
value tracker: which registers hold a constant, which hold an incoming
argument (copies through `move` and the stack included), so loads and
stores can be put down as "argument N + offset" or "hardware register X".
Approximate on purpose: the rules ask coarse questions (does it touch VI
registers, store at a0 + 8, call this and that) that a straight-line view
answers for every libultra version seen."""
from dataclasses import dataclass, field

from .mips import Ins, SP, RA

HW_BLOCKS = {0x0404: 'SP', 0x0410: 'DP', 0x0430: 'MI', 0x0440: 'VI', 0x0450: 'AI', 0x0460: 'PI', 0x0470: 'RI',
             0x0480: 'SI', 0x1FC0: 'PIF'}


@dataclass
class Feat:
    start: int
    size: int
    calls: list = field(default_factory=list)   # targets in order
    tails: set = field(default_factory=set)     # j out of the function (a tail call: GCC-built libultra, Mario Tennis)
    cop0: set = field(default_factory=set)      # ('mf'|'mt', reg)
    fcr: set = field(default_factory=set)
    cache: set = field(default_factory=set)     # cache op codes
    special: set = field(default_factory=set)   # eret tlbp tlbr tlbwi sync div 64
    hw: dict = field(default_factory=dict)      # physical HW address -> 'r'|'w'|'rw'
    k1ram: bool = False                         # RDRAM by a constant uncached (KSEG1) address: a memory-size probe
    arg: dict = field(default_factory=dict)     # (argno, offset, size) -> 'r'|'w'|'rw'
    consts: set = field(default_factory=set)    # immediates and lui values formed
    globals_: dict = field(default_factory=dict)  # constant RAM address -> 'r'|'w'|'rw'
    args_used: set = field(default_factory=set)   # a0..a3 read before written
    frame: int = 0                              # stack frame size
    returns_value: bool = False
    gptr: dict = field(default_factory=dict)    # (global, offset, size) -> rw, through a pointer loaded from it
    gptr_src: dict = field(default_factory=dict)  # (global, offset) -> what was stored: ('arg', n) | ('const', v)
    addr_taken: set = field(default_factory=set)  # constants formed in the code (function pointers among them)
    float_args: bool = False                    # reads $f12/$f14 before writing them
    divs: set = field(default_factory=set)      # mflo/mfhi after which div kind: 'ddivu' 'ddiv' 'dmultu' ...
    shifts64: set = field(default_factory=set)  # 'dsllv' 'dsrlv' 'dsrav'
    branches: int = 0
    cvt_l: set = field(default_factory=set)     # COP1 conversions from a 64-bit integer: 's' | 'd'

    @property
    def leaf(self):
        return not self.calls

    def hw_blocks(self):
        return {HW_BLOCKS.get(a >> 16, '?') for a in self.hw}

    def touches(self, block):
        return block in self.hw_blocks()


def _sources(i):
    """the general registers an instruction reads"""
    op = i.op
    if op == 0:
        if i.funct in (0, 2, 3):              # sll srl sra: rt only
            return (i.rt,)
        return (i.rs, i.rt)
    if op in (4, 5, 20, 21) or i.is_store:    # beq bne beql bnel, stores
        return (i.rs, i.rt)
    if op in (2, 3, 15):                      # j jal lui
        return ()
    if op in (16, 17):                        # mtc0 / mtc1 / ctc1 read rt
        return (i.rt,) if i.rs in (4, 6) else ()
    return (i.rs,)


def _mark(d, k, rw):
    old = d.get(k, '')
    if rw not in old:
        d[k] = ''.join(sorted(old + rw, reverse=True))


def features(img, f):
    ft = Feat(f.start, f.end - f.start)
    ft.tails = set(getattr(f, 'tails', ()))
    const = {0: 0}                 # reg -> value
    gload = {}                     # reg -> global the register's value was loaded from
    fwritten = set()
    lastdiv = None
    clobber_after = None
    argof = {4: 0, 5: 1, 6: 2, 7: 3}   # reg -> incoming argument number
    slot = {}                      # sp offset -> ('arg', n) | ('const', v)
    written = set()
    pc = f.start
    while pc < f.end:
        w = img.word(pc)
        if w is None:
            break
        i = Ins(w, pc)
        for r in _sources(i):   # argument registers read before any write
            if 4 <= r <= 7 and r not in written:
                ft.args_used.add(r - 4)
        if i.op == 9 and i.rs == SP and i.rt == SP and i.simm < 0 and not ft.frame:
            ft.frame = -i.simm
        c = i.cop0
        if c:
            ft.cop0.add(c)
        if i.fcr:
            ft.fcr.add(i.fcr)
        if i.op == 47:
            ft.cache.add(i.rt)
        if i.w == 0x42000018:
            ft.special.add('eret')
        elif i.w in (0x42000008, 0x42000001, 0x42000002, 0x42000006):
            ft.special.add({0x42000008: 'tlbp', 0x42000001: 'tlbr', 0x42000002: 'tlbwi', 0x42000006: 'tlbwr'}[i.w])
        if i.op == 0 and i.funct == 15:
            ft.special.add('sync')
        if i.op == 0 and i.funct in (26, 27, 30, 31):
            ft.special.add('div')
        if i.op == 0 and i.funct in (20, 22, 23):
            ft.shifts64.add({20: 'dsllv', 22: 'dsrlv', 23: 'dsrav'}[i.funct])
        if i.is_branch:
            ft.branches += 1
        if i.op == 17 and i.rs == 21 and i.funct in (32, 33):   # cvt.s.l / cvt.d.l
            ft.cvt_l.add('s' if i.funct == 32 else 'd')
        if i.op == 0 and i.funct in (28, 29, 30, 31):   # dmult dmultu ddiv ddivu
            lastdiv = {28: 'dmult', 29: 'dmultu', 30: 'ddiv', 31: 'ddivu'}[i.funct]
        if i.op == 0 and i.funct in (16, 18) and lastdiv:   # mfhi / mflo
            ft.divs.add((lastdiv, 'hi' if i.funct == 16 else 'lo'))
        if i.op == 17 and i.rs in (16, 17) and i.funct not in (32, 33, 36) or i.op in (49, 53, 57, 61):
            pass
        if i.op == 17 and i.rs in (16, 17):            # COP1 arithmetic: fs (rd field) and ft read
            for fr in (i.rd, i.rt):
                if fr in (12, 14) and fr not in fwritten:
                    ft.float_args = True
            fwritten.add(i.sa)                          # fd
        if i.op == 17 and i.rs == 4:                    # mtc1 writes fs
            fwritten.add(i.rd)
        if i.op == 17 and i.rs == 0 and i.rd in (12, 14) and i.rd not in fwritten:   # mfc1 reads fs
            ft.float_args = True
        if i.op in (57, 61) and i.rt in (12, 14) and i.rt not in fwritten:          # swc1/sdc1 of an arg
            ft.float_args = True
        if i.is_64:
            ft.special.add('64')
        if i.is_jal:
            ft.calls.append(i.target)
        if i.op in (15, 13, 9) and (i.op != 9 or i.rs == 0):
            ft.consts.add(i.imm << 16 if i.op == 15 else i.imm)
        # memory
        if i.is_load or i.is_store:
            rw = 'w' if i.is_store else 'r'
            if i.rs == SP:
                if i.is_store and i.op == 43:
                    if i.rt in argof:
                        slot[i.simm] = ('arg', argof[i.rt])
                    elif i.rt in const:
                        slot[i.simm] = ('const', const[i.rt])
                    else:
                        slot.pop(i.simm, None)
            elif i.rs in argof:
                _mark(ft.arg, (argof[i.rs], i.simm, i.mem_size), rw)
            elif i.rs in gload:
                _mark(ft.gptr, (gload[i.rs], i.simm, i.mem_size), rw)
                if i.is_store:
                    src = ('arg', argof[i.rt]) if i.rt in argof else ('const', const[i.rt]) if i.rt in const else ('?',)
                    ft.gptr_src[(gload[i.rs], i.simm)] = src
            elif i.rs in const:
                a = (const[i.rs] + i.simm) & 0xFFFFFFFF
                if 0xA4000000 <= a < 0xA4900000 or 0xBFC00000 <= a < 0xBFD00000 or 0xBFF00000 <= a < 0xC0000000:
                    # (0xBFF00000: a development board's ID, "KMC" on Kyoto Microcomputer's; Mario Party 3 probes
                    # it at boot. The interpreter's model reads 0 there, as a retail cartridge bus does)
                    _mark(ft.hw, a & 0x1FFFFFFF, rw)
                elif 0x80000000 <= a < 0x80800000:
                    _mark(ft.globals_, a, rw)
                elif 0xA0000000 <= a < 0xA0800000:
                    ft.k1ram = True
        # value tracking
        d = i.dest
        if i.is_jal or i.is_jalr:
            clobber_after = pc + 4   # the delay slot runs before the callee clobbers anything
            pc += 4
            continue
        if d is not None:
            v, a, gl = None, None, None
            if i.op == 15:
                v = i.imm << 16
            elif i.op in (8, 9) and i.rs in const:
                v = const[i.rs] + i.simm
            elif i.op == 13 and i.rs in const:
                v = const[i.rs] | i.imm
            elif i.op == 0 and i.funct == 37 and i.rt == 0:       # move d, rs
                v, a = const.get(i.rs), argof.get(i.rs)
            elif i.op == 0 and i.funct == 33 and i.rt == 0:       # addu d, rs, $0
                v, a = const.get(i.rs), argof.get(i.rs)
            elif i.op == 35 and i.rs in const and 0x80000000 <= (const[i.rs] + i.simm) & 0xFFFFFFFF < 0x80800000:
                gl = (const[i.rs] + i.simm) & 0xFFFFFFFF   # lw d, global: a pointer read from a global
            elif i.op == 35 and i.rs == SP and i.simm in slot:    # lw d, n(sp): a saved argument
                kind, val = slot[i.simm]
                if kind == 'arg':
                    a = val
                else:
                    v = val
            if d in (2, 3):
                ft.returns_value = True
            gaddr = None
            if i.op == 35 and i.rs in const and 0x80000000 <= (const[i.rs] + i.simm) & 0xFFFFFFFF < 0x80800000:
                gaddr = (const[i.rs] + i.simm) & 0xFFFFFFFF   # before the base register is overwritten
            written.add(d)
            const.pop(d, None)
            argof.pop(d, None)
            gload.pop(d, None)
            if gaddr is not None:
                gload[d] = gaddr
            if v is not None:
                const[d] = v & 0xFFFFFFFF
                if i.op in (9, 13) and 0x80000000 <= const[d] < 0x80800000:
                    ft.addr_taken.add(const[d])
            if a is not None:
                argof[d] = a
        if clobber_after == pc:
            for r in (2, 3, 4, 5, 6, 7, *range(8, 16), 24, 25, RA):
                const.pop(r, None)
                argof.pop(r, None)
                gload.pop(r, None)
            written.update((2, 3, 4, 5, 6, 7))
            clobber_after = None
        pc += 4
    return ft
