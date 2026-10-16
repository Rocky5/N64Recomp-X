#!/usr/bin/env python3
"""Build the ROM-free end-to-end test image (PLAN.md 4.6): a tiny N64 ROM
assembled here from the program below, plus its symbol file.

  tools/xbox/make_test_rom.py OUT.z64 [--syms games/test/test.syms.toml]

The plan named a libdragon-built ROM; this is the same idea without a MIPS
toolchain on three operating systems: a self-checking MIPS program assembled
by the ~150 lines below, so the image (and its SHA-1, which the test pack
lists) is the same byte for byte everywhere (docs/decisions.md). It uses no
libultra and contains no Nintendo code: the IPL3 area is zeros, the header is
made up, and nothing in it is ever run on an N64.

What it exercises: the recompiler on hand-written IDO-shaped code (a loop
with a delay slot, calls, a function pointer through LOOKUP_FUNC, an IDO
jump table, mult/div, shifts and compares, byte/halfword/unaligned memory
over the word-swapped image, single and double floats with rounding-mode
changes), then our recomp.h and the RDRAM window on the console. Each check
calls `check(actual, expected)`, which counts steps and failures in the
result block at 0x80100000: magic "NRX1" at +0 (written last), failures at
+4, steps at +8, the last failing value at +12. The runtime's test boot
(xbox/src/rt/boot.c) runs the entrypoint and shows the block on a card.
"""
import argparse
import hashlib
import pathlib
import struct
import sys

REGS = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}
VRAM = 0x80000400
ROM_CODE = 0x1000
ROM_SIZE = 0x100000
RES = 0x80100000
MAGIC = 0x4E525831   # "NRX1"


def u32(x):
    return x & 0xFFFFFFFF


def hi16(x):   # %hi with the carry of the sign-extended %lo
    return ((x >> 16) + ((x >> 15) & 1)) & 0xFFFF


def lo16(x):
    return x & 0xFFFF


class Asm:
    def __init__(self, base):
        self.base = base
        self.items = []    # (kind, payload)
        self.labels = {}
        self.funcs = []    # (name, start label)
        self.checks = 0

    def here(self):
        return self.base + 4 * len(self.items)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.here()

    def func(self, name):
        self.funcs.append(name)
        self.label(name)

    def ins(self, op, *args):
        self.items.append((op, args))

    def word(self, value):
        self.items.append(('.word', (value,)))

    # ---- encoding ----
    def addr(self, a):
        return self.labels[a] if isinstance(a, str) else a

    def enc(self, pc, op, a):
        r = REGS
        R = lambda rs, rt, rd, sh, fn: (r[rs] << 21) | (r[rt] << 16) | (r[rd] << 11) | (sh << 6) | fn
        I = lambda opc, rs, rt, imm: (opc << 26) | (r[rs] << 21) | (r[rt] << 16) | (imm & 0xFFFF)
        def imm(v):
            if isinstance(v, tuple):   # ('hi', label) / ('lo', label)
                kind, lab = v
                x = self.addr(lab)
                return hi16(x) if kind == 'hi' else lo16(x)
            return v
        def br(target):
            off = (self.addr(target) - (pc + 4)) >> 2
            assert -0x8000 <= off < 0x8000
            return off & 0xFFFF
        rtype = {'addu': 0x21, 'subu': 0x23, 'and': 0x24, 'or': 0x25, 'xor': 0x26, 'nor': 0x27,
                 'slt': 0x2A, 'sltu': 0x2B}
        if op == '.word':
            return u32(self.addr(a[0]))
        if op == 'nop':
            return 0
        if op in rtype:
            return R(a[1], a[2], a[0], 0, rtype[op])
        if op == 'move':
            return R(a[1], 'zero', a[0], 0, 0x21)
        if op in ('sll', 'srl', 'sra'):
            return R('zero', a[1], a[0], a[2], {'sll': 0, 'srl': 2, 'sra': 3}[op])
        if op in ('sllv', 'srlv', 'srav'):   # rd, rt, rs
            return R(a[2], a[1], a[0], 0, {'sllv': 4, 'srlv': 6, 'srav': 7}[op])
        if op == 'jr':
            return R(a[0], 'zero', 'zero', 0, 8)
        if op == 'jalr':   # jalr rs (rd = ra)
            return R(a[0], 'zero', 'ra', 0, 9)
        if op in ('mult', 'multu', 'div', 'divu'):
            return R(a[0], a[1], 'zero', 0, {'mult': 0x18, 'multu': 0x19, 'div': 0x1A, 'divu': 0x1B}[op])
        if op in ('mfhi', 'mflo'):
            return R('zero', 'zero', a[0], 0, {'mfhi': 0x10, 'mflo': 0x12}[op])
        itype = {'addiu': 9, 'slti': 10, 'sltiu': 11, 'andi': 12, 'ori': 13, 'xori': 14}
        if op in itype:
            return I(itype[op], a[1], a[0], imm(a[2]))
        if op == 'lui':
            return I(15, 'zero', a[0], imm(a[1]))
        mem = {'lb': 0x20, 'lh': 0x21, 'lwl': 0x22, 'lw': 0x23, 'lbu': 0x24, 'lhu': 0x25, 'lwr': 0x26,
               'sb': 0x28, 'sh': 0x29, 'sw': 0x2B}
        if op in mem:   # rt, offset, base
            return I(mem[op], a[2], a[0], imm(a[1]))
        if op in ('beq', 'bne'):
            return I(4 if op == 'beq' else 5, a[0], a[1], br(a[2]))
        if op in ('blez', 'bgtz'):
            return I(6 if op == 'blez' else 7, a[0], 'zero', br(a[1]))
        if op in ('bltz', 'bgez'):
            return (1 << 26) | (r[a[0]] << 21) | ((0 if op == 'bltz' else 1) << 16) | br(a[1])
        if op in ('j', 'jal'):
            return ((2 if op == 'j' else 3) << 26) | ((self.addr(a[0]) >> 2) & 0x3FFFFFF)
        # COP1: fmt S = 16, D = 17, W = 20
        f = lambda s: int(s[1:])
        if op in ('mtc1', 'mfc1'):   # rt, fs
            return (0x11 << 26) | ((4 if op == 'mtc1' else 0) << 21) | (r[a[0]] << 16) | (f(a[1]) << 11)
        if op in ('ctc1', 'cfc1'):   # rt, fs(31)
            return (0x11 << 26) | ((6 if op == 'ctc1' else 2) << 21) | (r[a[0]] << 16) | (a[1] << 11)
        fpu = {'add': 0, 'sub': 1, 'mul': 2, 'div': 3, 'trunc.w': 13, 'cvt.s': 32, 'cvt.d': 33, 'cvt.w': 36}
        name, fmt = op.rsplit('.', 1)
        fmts = {'s': 16, 'd': 17, 'w': 20}[fmt]
        fn = fpu[name]
        if name in ('add', 'sub', 'mul', 'div'):   # fd, fs, ft
            return (0x11 << 26) | (fmts << 21) | (f(a[2]) << 16) | (f(a[1]) << 11) | (f(a[0]) << 6) | fn
        return (0x11 << 26) | (fmts << 21) | (f(a[1]) << 11) | (f(a[0]) << 6) | fn   # fd, fs

    def assemble(self):
        out = bytearray()
        for i, (op, a) in enumerate(self.items):
            out += struct.pack('>I', self.enc(self.base + 4 * i, op, a))
        return bytes(out)

    def func_sizes(self):
        starts = sorted(self.labels[n] for n in self.funcs)
        end = self.labels['__code_end']
        return [(n, self.labels[n], (starts[starts.index(self.labels[n]) + 1] if self.labels[n] != starts[-1] else end)
                 - self.labels[n]) for n in self.funcs]


def li(a, reg, value):
    value = u32(value)
    if value < 0x8000:
        a.ins('addiu', reg, 'zero', value)
    elif value >= 0xFFFF8000:
        a.ins('addiu', reg, 'zero', value - 0x100000000)
    else:
        a.ins('lui', reg, value >> 16)
        if value & 0xFFFF:
            a.ins('ori', reg, reg, value & 0xFFFF)


def check(a, reg, expected):
    """check(reg, expected): a0 = reg, a1 = expected"""
    a.ins('move', 'a0', reg)
    li(a, 'a1', expected)
    a.ins('jal', 'check')
    a.ins('nop')
    a.checks += 1


def program():
    a = Asm(VRAM)
    # ---- entry: the checks in order ----
    a.func('entry')
    a.ins('addiu', 'sp', 'sp', -64)
    a.ins('sw', 'ra', 60, 'sp')
    a.ins('sw', 's0', 56, 'sp')
    a.ins('sw', 's1', 52, 'sp')
    a.ins('lui', 's1', RES >> 16)
    a.ins('sw', 'zero', 0, 's1')
    a.ins('sw', 'zero', 4, 's1')
    a.ins('sw', 'zero', 8, 's1')
    # a loop with a delay slot: fib(20)
    a.ins('jal', 'fib')
    a.ins('addiu', 'a0', 'zero', 20)
    check(a, 'v0', 6765)
    # a function pointer: jalr through LOOKUP_FUNC
    a.ins('lui', 't9', ('hi', 'add3'))
    a.ins('addiu', 't9', 't9', ('lo', 'add3'))
    a.ins('addiu', 'a0', 'zero', 1000)
    a.ins('jalr', 't9')
    a.ins('addiu', 'a1', 'zero', 234)
    check(a, 'v0', 1237)
    # an IDO jump table
    for n, want in ((2, 33), (0, 11), (3, 44), (7, -1)):
        a.ins('jal', 'sw_case')
        a.ins('addiu', 'a0', 'zero', n)
        check(a, 'v0', want)
    # byte order over the word-swapped image
    li(a, 't0', 0x11223344)
    a.ins('sw', 't0', 0, 'sp')
    a.ins('lbu', 's0', 0, 'sp'); check(a, 's0', 0x11)
    a.ins('lbu', 's0', 3, 'sp'); check(a, 's0', 0x44)
    a.ins('lhu', 's0', 2, 'sp'); check(a, 's0', 0x3344)
    a.ins('addiu', 't0', 'zero', 0xAB)
    a.ins('sb', 't0', 1, 'sp')
    a.ins('lw', 's0', 0, 'sp'); check(a, 's0', 0x11AB3344)
    a.ins('lb', 's0', 1, 'sp'); check(a, 's0', 0xFFFFFFAB)
    li(a, 't0', 0xBEEF)
    a.ins('sh', 't0', 2, 'sp')
    a.ins('lw', 's0', 0, 'sp'); check(a, 's0', 0x11ABBEEF)
    a.ins('lh', 's0', 2, 'sp'); check(a, 's0', 0xFFFFBEEF)
    # unaligned: lwl/lwr across two words
    li(a, 't0', 0x11223344); a.ins('sw', 't0', 8, 'sp')
    li(a, 't0', 0x55667788); a.ins('sw', 't0', 12, 'sp')
    a.ins('addiu', 's0', 'zero', 0)
    a.ins('lwl', 's0', 9, 'sp')
    a.ins('lwr', 's0', 12, 'sp')
    check(a, 's0', 0x22334455)
    # multiply and divide
    li(a, 't0', 12345); li(a, 't1', 6789)
    a.ins('mult', 't0', 't1'); a.ins('mflo', 's0'); check(a, 's0', 12345 * 6789)
    li(a, 't0', 0xFFFFFFFF)
    a.ins('multu', 't0', 't0')
    a.ins('mfhi', 's0'); check(a, 's0', 0xFFFFFFFE)
    a.ins('mflo', 's0'); check(a, 's0', 1)
    li(a, 't0', -100); li(a, 't1', 7)
    a.ins('div', 't0', 't1')
    a.ins('mflo', 's0'); check(a, 's0', -14)
    a.ins('mfhi', 's0'); check(a, 's0', -2)
    li(a, 't0', 1000003); li(a, 't1', 97)
    a.ins('divu', 't0', 't1')
    a.ins('mflo', 's0'); check(a, 's0', 1000003 // 97)
    a.ins('mfhi', 's0'); check(a, 's0', 1000003 % 97)
    # shifts and compares
    li(a, 't0', -256)
    a.ins('sra', 's0', 't0', 4); check(a, 's0', -16)
    a.ins('srl', 's0', 't0', 4); check(a, 's0', 0x0FFFFFF0)
    a.ins('addiu', 't1', 'zero', 3)
    a.ins('sllv', 's0', 't1', 't1'); check(a, 's0', 24)
    li(a, 't0', -1); li(a, 't1', 1)
    a.ins('slt', 's0', 't0', 't1'); check(a, 's0', 1)
    a.ins('sltu', 's0', 't0', 't1'); check(a, 's0', 0)
    # a count-down loop on bgtz: 5 + 4 + 3 + 2 + 1
    a.ins('addiu', 't0', 'zero', 5)
    a.ins('addiu', 's0', 'zero', 0)
    a.label('sum_loop')
    a.ins('addu', 's0', 's0', 't0')
    a.ins('addiu', 't0', 't0', -1)
    a.ins('bgtz', 't0', 'sum_loop')
    a.ins('nop')
    check(a, 's0', 15)
    # single floats and the rounding mode (MXCSR)
    a.ins('addiu', 't0', 'zero', 7)
    a.ins('mtc1', 't0', 'f0')
    a.ins('cvt.s.w', 'f0', 'f0')            # 7.0
    a.ins('mul.s', 'f2', 'f0', 'f0')        # 49.0
    li(a, 't0', 0x3F000000)                 # 0.5f
    a.ins('mtc1', 't0', 'f4')
    a.ins('add.s', 'f6', 'f2', 'f4')        # 49.5
    a.ins('cvt.w.s', 'f8', 'f6'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', 50)   # to nearest even
    a.ins('trunc.w.s', 'f8', 'f6'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', 49)
    a.ins('addiu', 't0', 'zero', 1)         # RM toward zero
    a.ins('ctc1', 't0', 31)
    a.ins('cvt.w.s', 'f8', 'f6'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', 49)
    a.ins('sub.s', 'f10', 'f4', 'f6')       # 0.5 - 49.5 = -49.0
    a.ins('sub.s', 'f10', 'f10', 'f4')      # -49.5
    a.ins('addiu', 't0', 'zero', 3)         # RM down
    a.ins('ctc1', 't0', 31)
    a.ins('cvt.w.s', 'f8', 'f10'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', -50)
    # doubles on the x87, still rounding down
    a.ins('cvt.d.s', 'f12', 'f10')          # -49.5
    a.ins('cvt.w.d', 'f8', 'f12'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', -50)
    a.ins('ctc1', 'zero', 31)               # RM nearest
    a.ins('cvt.d.s', 'f12', 'f0')           # 7.0
    a.ins('mul.d', 'f14', 'f12', 'f12')     # 49.0
    a.ins('cvt.w.d', 'f8', 'f14'); a.ins('mfc1', 's0', 'f8'); check(a, 's0', 49)
    a.ins('cfc1', 's0', 31); check(a, 's0', 0)
    # done: the magic last, so a crash halfway leaves none
    li(a, 't0', MAGIC)
    a.ins('sw', 't0', 0, 's1')
    a.ins('lw', 'ra', 60, 'sp')
    a.ins('lw', 's0', 56, 'sp')
    a.ins('lw', 's1', 52, 'sp')
    a.ins('jr', 'ra')
    a.ins('addiu', 'sp', 'sp', 64)

    # ---- check(a0 actual, a1 expected): steps++, failures++ on a mismatch;
    # only a2/a3 are touched, so the checks in entry keep their t registers ----
    a.func('check')
    a.ins('lw', 'a2', 8, 's1')
    a.ins('addiu', 'a2', 'a2', 1)
    a.ins('beq', 'a0', 'a1', 'check_ok')
    a.ins('sw', 'a2', 8, 's1')
    a.ins('lw', 'a3', 4, 's1')
    a.ins('addiu', 'a3', 'a3', 1)
    a.ins('sw', 'a3', 4, 's1')
    a.ins('sw', 'a0', 12, 's1')
    a.label('check_ok')
    a.ins('jr', 'ra')
    a.ins('nop')

    # ---- fib(n), iterative ----
    a.func('fib')
    a.ins('addiu', 'v0', 'zero', 0)
    a.ins('beq', 'a0', 'zero', 'fib_done')
    a.ins('addiu', 'v1', 'zero', 1)
    a.label('fib_loop')
    a.ins('addu', 't0', 'v0', 'v1')
    a.ins('move', 'v0', 'v1')
    a.ins('addiu', 'a0', 'a0', -1)
    a.ins('bne', 'a0', 'zero', 'fib_loop')
    a.ins('move', 'v1', 't0')
    a.label('fib_done')
    a.ins('jr', 'ra')
    a.ins('nop')

    # ---- add3(a, b) = a + b + 3, called through a pointer ----
    a.func('add3')
    a.ins('addu', 'v0', 'a0', 'a1')
    a.ins('jr', 'ra')
    a.ins('addiu', 'v0', 'v0', 3)

    # ---- sw_case(n): IDO's jump-table shape ----
    a.func('sw_case')
    a.ins('sltiu', 'at', 'a0', 4)
    a.ins('beq', 'at', 'zero', 'sw_default')
    a.ins('sll', 't6', 'a0', 2)
    a.ins('lui', 'at', ('hi', 'jtbl'))
    a.ins('addu', 'at', 'at', 't6')
    a.ins('lw', 't6', ('lo', 'jtbl'), 'at')
    a.ins('jr', 't6')
    a.ins('nop')
    for i, v in enumerate((11, 22, 33, 44)):
        a.label(f'case{i}')
        a.ins('jr', 'ra')
        a.ins('addiu', 'v0', 'zero', v)
    a.label('sw_default')
    a.ins('jr', 'ra')
    a.ins('addiu', 'v0', 'zero', -1)
    a.label('__code_end')
    # ---- data ----
    a.label('jtbl')
    for i in range(4):
        a.word(f'case{i}')
    a.label('__data_end')
    return a


def build():
    a = program()
    code = a.assemble()
    hdr = bytearray(0x40)
    struct.pack_into('>IIII', hdr, 0, 0x80371240, 0x0000000F, VRAM, 0x00001444)
    hdr[0x20:0x34] = b'N64RECOMP-X TEST    '
    hdr[0x3B:0x3F] = b'NXTE'
    rom = bytearray(ROM_SIZE)
    rom[:0x40] = hdr
    rom[ROM_CODE:ROM_CODE + len(code)] = code
    crc = hashlib.sha1(bytes(rom[0x1000:])).digest()
    hdr_crc = struct.unpack('>II', crc[:8])   # made up: nothing checks it, but it is stable
    struct.pack_into('>II', rom, 0x10, *hdr_crc)
    return a, bytes(rom), len(code)


def syms_toml(a, size):
    lines = ['# The test ROM\'s symbols (tools/xbox/make_test_rom.py writes this file).', '',
             '[[section]]', 'name = ".text"', f'rom = 0x{ROM_CODE:08X}', f'vram = 0x{VRAM:08X}',
             f'size = 0x{size:X}', '', 'functions = [']
    for name, vram, fsize in a.func_sizes():
        lines.append(f'    {{ name = "{name}", vram = 0x{vram:08X}, size = 0x{fsize:X} }},')
    lines += [']', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out', help='the .z64 to write')
    ap.add_argument('--syms', help='also write the symbol file here')
    ap.add_argument('--print-sha1', action='store_true')
    args = ap.parse_args()
    a, rom, size = build()
    pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(args.out).write_bytes(rom)
    if args.syms:
        pathlib.Path(args.syms).write_text(syms_toml(a, size), newline='\n')
    sha = hashlib.sha1(rom).hexdigest()
    print(f'{args.out}: {len(rom)} bytes, {size} bytes of code, {a.checks} checks, sha1 {sha}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
