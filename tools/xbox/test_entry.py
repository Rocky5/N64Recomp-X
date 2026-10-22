#!/usr/bin/env python3
"""The analyser's entry-code shapes (analyze/funcs.py parse_entry) on
hand-assembled entries: no ROM. Each shape is one a toolchain wrote: the
SDK's (start + size, jr), a start-to-end clear with jal main, empty clear
loops before jal main, the stack and an immediate j or jal, and something
that is none of these."""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib.analyze import funcs  # noqa: E402

ENTRY = 0x80000400
T0, T1, T2, SP, AT, ZERO = 8, 9, 10, 29, 1, 0
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f'test_entry: FAIL {what}')


def i_(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def lui(rt, imm): return i_(15, 0, rt, imm)
def addiu(rt, rs, imm): return i_(9, rs, rt, imm)
def ori(rt, rs, imm): return i_(13, rs, rt, imm)
def sw(rt, off, base): return i_(43, base, rt, off)
def beq(rs, rt, off): return i_(4, rs, rt, off)
def bne(rs, rt, off): return i_(5, rs, rt, off)
def sltu(rd, rs, rt): return (rs << 21) | (rt << 16) | (rd << 11) | 0x2B
def jr(rs): return (rs << 21) | 8
def j(t): return (2 << 26) | ((t >> 2) & 0x3FFFFFF)
def jal(t): return (3 << 26) | ((t >> 2) & 0x3FFFFFF)


NOP = 0


def rom_with(words):
    rom = bytearray(0x101000)
    rom[8:12] = struct.pack('>I', ENTRY)
    for k, w in enumerate(words):
        rom[0x1000 + 4 * k:0x1004 + 4 * k] = struct.pack('>I', w)
    return bytes(rom)


def hi(v): return (v >> 16) + (1 if v & 0x8000 else 0)
def lo(v): return v & 0xFFFF


def main():
    bss, size, mainf = 0x80040000, 0x1230, 0x80001000
    # the SDK: t0 = start (lui/addiu), t1 = size (lui/ori), clear 8 at a time, t2 = main, jr t2
    sdk = [lui(T0, hi(bss)), addiu(T0, T0, lo(bss)), lui(T1, size >> 16), ori(T1, T1, size & 0xFFFF),
           addiu(T1, T1, -8), sw(ZERO, 0, T0), sw(ZERO, 4, T0), bne(T1, ZERO, -4), addiu(T0, T0, 8),
           lui(T2, hi(mainf)), addiu(T2, T2, lo(mainf)), lui(SP, 0x8004), jr(T2), addiu(SP, SP, 0)]
    r = funcs.parse_entry(rom_with(sdk), ENTRY)   # (the size, a note only, is read after the loop's first decrement)
    check(r is not None and r[0] == bss and r[2] == mainf, 'the SDK entry')

    # start-to-end with sw zero, -4(t0) after the increment, then jal main (Forsaken 64's shape)
    end = bss + 0x4000
    se = [lui(SP, 0x8004), addiu(SP, SP, 0x100), lui(T0, hi(bss)), addiu(T0, T0, lo(bss)),
          lui(T1, hi(end)), addiu(T1, T1, lo(end)), beq(T0, T1, 5), NOP, addiu(T0, T0, 4), sltu(AT, T0, T1),
          bne(AT, ZERO, -3), sw(ZERO, -4, T0), jal(mainf), NOP, beq(ZERO, ZERO, -1), NOP]
    check(funcs.parse_entry(rom_with(se), ENTRY) == (bss, 0x4000, mainf), 'a start-to-end clear and jal main')

    # two empty clear loops (start == end), then jal main: no BSS, IPL3's 1 MB (Extreme-G 2)
    e0 = 0x80000400
    empty = [lui(SP, 0x8004), ori(SP, SP, 0), lui(T0, 0x8000), addiu(T0, T0, 0x400), lui(T1, 0x8000),
             addiu(T1, T1, 0x400), beq(T0, T1, 5), NOP, addiu(T0, T0, 4), sltu(AT, T0, T1), bne(AT, ZERO, -3),
             sw(ZERO, -4, T0), jal(mainf), NOP]
    check(funcs.parse_entry(rom_with(empty), ENTRY) == (ENTRY + 0x100000, 0, mainf), 'empty clears, then jal main')
    del e0

    # the stack, then j at once (Rogue Squadron) / jal at once (Madden 2000)
    check(funcs.parse_entry(rom_with([lui(SP, 0x803F), j(mainf), ori(SP, SP, 0xFFF0)]), ENTRY)
          == (ENTRY + 0x100000, 0, mainf), 'the stack and j')
    check(funcs.parse_entry(rom_with([lui(SP, 0x803F), ori(SP, SP, 0xFFF0), jal(mainf), NOP]), ENTRY)
          == (ENTRY + 0x100000, 0, mainf), 'the stack and jal')

    # the stack, then jr to a loader in the boot segment (Turok, Episode I Racer)
    T2_ = 10
    check(funcs.parse_entry(rom_with([lui(T2_, 0x8000), lui(SP, 0x8015), addiu(T2_, T2_, 0x4C4), jr(T2_),
                                      addiu(SP, SP, -0x4900)]), ENTRY) == (ENTRY + 0x100000, 0, 0x800004C4),
          'the stack and jr to a loader')
    # set-up calls, then main, then break (Army Men)
    brk = 0x0001008D
    check(funcs.parse_entry(rom_with([lui(4, 0x800B), jal(0x800004A0), NOP, jal(0x800004A0), NOP, jal(mainf), NOP,
                                      brk]), ENTRY) == (ENTRY + 0x100000, 0, mainf), 'set-up calls, main, break')

    # none of these: a jal before any stack or clear (a loader of its own) is not taken for main
    check(funcs.parse_entry(rom_with([jal(mainf), NOP] + [NOP] * 40), ENTRY) is None, 'a bare jal is refused')
    check(funcs.parse_entry(rom_with([NOP] * 48), ENTRY) is None, 'no entry at all')

    test_names()
    print(f'test_entry: {"passed" if not fails else f"{fails} failed"}')
    return 1 if fails else 0


def test_names():
    """libultra.py's naming (docs/review-2026-10.md 4.1, 4.2): one fit is
    named; a second fit of a different shape is reported (the probe pass),
    a copy of the same shape is not; with N64RX_NAME_UNIQUE the nearest to
    the other names wins"""
    from n64rx_lib.analyze import libultra
    V0, RA = 2, 31

    def mfc0(rt, rd): return (16 << 26) | (rt << 16) | (rd << 11)
    count = [mfc0(V0, 9), jr(RA), NOP, NOP]                 # osGetCount
    other = [mfc0(V0, 9), jr(RA), addiu(V0, V0, 1), NOP]    # a game's leaf of the same kind, another shape

    def ident(*bodies, unique=False):
        words = [w for b in bodies for w in b]
        img = funcs.Image(rom_with(words))
        img.segments.append(funcs.Segment('boot', 0x1000, ENTRY, 4 * len(words)))
        fs = {ENTRY + 16 * k: funcs.Func(ENTRY + 16 * k, ENTRY + 16 * k + 12) for k in range(len(bodies))}
        old, libultra.NAME_UNIQUE = libultra.NAME_UNIQUE, unique
        try:
            ids, N = libultra.identify(img, fs)
        finally:
            libultra.NAME_UNIQUE = old
        return {a: n for a, (n, _w) in ids.items()}, N

    ids, N = ident(count)
    check(ids.get(ENTRY) == 'osGetCount' and not N.ambiguous, 'one fit: named, nothing ambiguous')
    ids, N = ident(count, other)
    check(list(ids.values()).count('osGetCount') == 1, 'two fits: one name')
    check(N.ambiguous.get('osGetCount') == [ENTRY, ENTRY + 16], f'two fits of two shapes: reported ({N.ambiguous})')
    ids, N = ident(count, count)
    check('osGetCount' not in N.ambiguous, 'a copy of the same shape: not ambiguous')
    ids, N = ident(count, other, unique=True)
    check(list(ids.values()).count('osGetCount') == 1, 'N64RX_NAME_UNIQUE: still one name')


if __name__ == '__main__':
    sys.exit(main())
