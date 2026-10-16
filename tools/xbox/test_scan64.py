#!/usr/bin/env python3
"""tools/xbox/recomp_scan64.py on hand-assembled functions (PLAN.md 3.2, 4.6):
it finds the 64-bit-ISA instructions (so a game that uses one runs with
64-bit registers), it finds `lui` of a KSEG1 or cartridge-domain address,
it ignores what the recompiler does not recompile (libultra's reimplemented
functions), and a plain O32 function has no hits. Needs rabbitizer."""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/xbox'))
import recomp_scan64  # noqa: E402

NOP = 0x00000000
JR_RA = 0x03E00008
ADDU_V0 = 0x00851021          # addu v0, a0, a1
DSLL32 = 0x0004203C           # dsll32 a0, a0, 0
LD = 0xDC820000               # ld v0, 0(a0)
DMULTU = 0x0085001D           # dmultu a0, a1
CVT_L_S = 0x46000025          # cvt.l.s f0, f0
LUI_A404 = 0x3C0FA404         # lui t7, 0xA404 (SP_STATUS_REG's page)
LUI_B000 = 0x3C08B000         # lui t0, 0xB000 (the cartridge domain)
LUI_8020 = 0x3C048020         # lui a0, 0x8020 (KSEG0: fine)


def image(funcs):
    words = []
    syms = {'section': [{'name': '.text', 'rom': 0x1000, 'vram': 0x80000400, 'size': 0, 'functions': []}]}
    vram = 0x80000400
    for name, body in funcs:
        syms['section'][0]['functions'].append({'name': name, 'vram': vram, 'size': 4 * len(body)})
        words += body
        vram += 4 * len(body)
    syms['section'][0]['size'] = 4 * len(words)
    rom = bytearray(0x1000) + b''.join(struct.pack('>I', w) for w in words)
    return bytes(rom), syms


def main():
    rom, syms = image([
        ('plain', [ADDU_V0, LUI_8020, JR_RA, NOP]),
        ('shift64', [DSLL32, JR_RA, NOP]),
        ('load64', [LD, JR_RA, NOP]),
        ('mul64', [DMULTU, JR_RA, NOP]),
        ('cvt64', [CVT_L_S, JR_RA, NOP]),
        ('kseg1', [LUI_A404, JR_RA, NOP]),
        ('cart', [LUI_B000, JR_RA, NOP]),
        ('osGetTime', [DSLL32, JR_RA, NOP]),   # reimplemented by the runtime: not counted
    ])
    r = recomp_scan64.scan(rom, syms)
    hits = {(f, op) for f, _, op in r['hits']}
    kseg1 = {f for f, _, _ in r['kseg1']}
    want = {('shift64', 'dsll32'), ('load64', 'ld'), ('mul64', 'dmultu'), ('cvt64', 'cvt.l.s')}
    bad = 0
    if hits != want:
        print(f'64-bit hits {sorted(hits)}, want {sorted(want)}')
        bad += 1
    if kseg1 != {'kseg1', 'cart'}:
        print(f'KSEG1 sites {sorted(kseg1)}, want cart and kseg1')
        bad += 1
    if r['functions'] != 7:
        print(f"{r['functions']} functions scanned, want 7 (osGetTime is the runtime's)")
        bad += 1
    rom2, syms2 = image([('plain', [ADDU_V0, LUI_8020, JR_RA, NOP])])
    if recomp_scan64.scan(rom2, syms2)['hits']:
        print('a plain O32 function has 64-bit hits')
        bad += 1
    print('test_scan64: ' + ('FAILED' if bad else 'passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
