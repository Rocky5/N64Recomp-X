#!/usr/bin/env python3
"""What the analyser hands the interpreter before N64Recomp runs
(analyze/emit.py recomp_hazards) and how captured RDRAM blocks join
(learn.py _code_runs), on hand-assembled words: no ROM. The jump-table
finder mirrors N64Recomp's analysis.cpp: lui, an addu of the index, lw, jr."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib.analyze import emit  # noqa: E402
from n64rx_lib import learn  # noqa: E402

AT, T6, T7, A0, SP, ZERO = 1, 14, 15, 4, 29, 0
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f'test_precheck: FAIL {what}')


def i_(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def r_(rs, rt, rd, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | funct


def lui(rt, imm): return i_(15, 0, rt, imm)
def addiu(rt, rs, imm): return i_(9, rs, rt, imm)
def addu(rd, rs, rt): return r_(rs, rt, rd, 33)
def lw(rt, imm, base): return i_(35, base, rt, imm)
def sw(rt, imm, base): return i_(43, base, rt, imm)
def jr(rs): return r_(rs, 0, 0, 8)
NOP = 0

# the usual shape: lui at, hi; addu at, at, index; lw t6, lo(at); jr t6
ws = [lui(AT, 0x8010), addu(AT, AT, T6), lw(T6, 0x1234, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80101234], 'lui/addu/lw/jr: the table at 0x80101234')

# a negative lo: hi 0x8011 + (-0x10)
ws = [lui(AT, 0x8011), addu(AT, T6, AT), lw(T6, -0x10, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x8010FFF0], 'a negative lo')

# addiu of the lo before the addu, lw with offset 0
ws = [lui(AT, 0x8010), addiu(AT, AT, 0x40), addu(AT, AT, T6), lw(T6, 0, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100040], 'addiu then lw 0')

# through the stack: sw then lw from sp keeps the state
ws = [lui(AT, 0x8010), addu(T7, AT, T6), sw(T7, 0x18, SP), lw(T7, 0x18, SP), lw(T6, 0x20, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100020], 'kept across the stack')

# a move (or rd, rs, $zero) keeps it; an addu of two lui registers loses it, as in N64Recomp
ws = [lui(AT, 0x8010), r_(AT, ZERO, T7, 37), addu(T7, T7, T6), lw(T6, 0, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [0x80100000], 'or keeps the lui')
ws = [lui(AT, 0x8010), lui(T7, 0x8020), addu(T7, T7, AT), lw(T6, 0, T7), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'addu of two lui registers')

# jr $ra is a return, not a table
ws = [lui(AT, 0x8010), addu(AT, AT, T6), lw(31, 0, AT), jr(31), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'jr ra')

# a write in between clears the register
ws = [lui(AT, 0x8010), addu(AT, AT, T6), i_(13, 0, AT, 5), lw(T6, 0, AT), jr(T6), NOP]
check(emit._n64recomp_tables(ws, 0x10000000) == [], 'ori overwrites the base')

# captured blocks: neighbours join, a gap starts a new run
code = {0x80280000: b'a' * 0x10000, 0x80290000: b'b' * 0x10000, 0x802B0000: b'c' * 0x10000}
runs = learn._code_runs(code)
check(sorted(runs) == [0x80280000, 0x802B0000], f'runs: {sorted(hex(x) for x in runs)}')
check(len(runs[0x80280000]) == 0x20000 and runs[0x80280000][0x10000:0x10001] == b'b', 'the joined run holds both blocks')

print('test_precheck: ' + ('ok' if not fails else f'{fails} failed'))
sys.exit(1 if fails else 0)
