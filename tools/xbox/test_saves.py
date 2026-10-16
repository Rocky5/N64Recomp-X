#!/usr/bin/env python3
"""Every N64 save type on the host (PLAN.md 3.6, 4.6): builds
tests/xbox/test_saves.c with xbox/src/rt/ultra/savedev.c (the paks at the
PIF, SRAM, the FlashRAM's commands), pfs.c (the runtime's osPfs*) and pi.c
(domain 2 through the PI path), the platform stubbed. Checks: libultra's two
pak CRCs against a second formulation, the blank pak's format as libultra
checks it, pak read/write round trips through the PIF commands, the Rumble
Pak's probe and the AUTO rule, the osPfs* file system (allocate, write, read,
state, find, delete, the checker on a broken entry, a blank pak repaired),
the BACK menu's Paks page (the note table, delete, copy, format with a new
ID, the status's pull bit and libultra's ID check after a swap),
the Flash's ID, erase, program and read, and SRAM by PI DMA."""
import sys

import hostcc

ROOT = hostcc.ROOT
sys.path.insert(0, str(ROOT / 'tools' / 'n64rx'))
from n64rx_lib import saves  # noqa: E402


def words(*ws):
    return b''.join(w.to_bytes(4, 'big') for w in ws)


def test_cont_max():
    """n64rx's find of __osMaxControllers in osContInit (hand-assembled)"""
    bad = []
    # addiu t7, zero, 4; lui at, 0x8019; jal x; sb t7, 0x6541(at)
    if saves.cont_max_store(words(0x240F0004, 0x3C018019, 0x0C000000, 0xA02F6541)) != (0x80196541, 1):
        bad.append('sb of 4 after a lui')
    # the register is reloaded before the store: not it
    if saves.cont_max_store(words(0x240F0004, 0x8FAF0010, 0x3C018019, 0xA02F6541, 0x03E00008)) is not None:
        bad.append('a reloaded register')
    # a negative offset, sw (the flag set to 1 first is skipped)
    if saves.cont_max_store(words(0x24080001, 0x3C01800F, 0xAC28B360, 0x24090004, 0x3C02801A,
                                  0xAC499ABF)) != (0x80199ABF, 4):
        bad.append('sw with a negative offset')
    for b in bad:
        print(f'  FAIL cont_max_store: {b}')
    return len(bad)


def main():
    nbad = test_cont_max()
    rc = hostcc.run(['tests/xbox/test_saves.c', 'xbox/src/rt/ultra/savedev.c', 'xbox/src/rt/ultra/pfs.c',
                     'xbox/src/rt/ultra/pi.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/rt/ultra'],
                    flags=['-Wno-unused-function', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_saves: ' + ('FAILED' if rc or nbad else 'passed'))
    return 1 if rc or nbad else 0


if __name__ == '__main__':
    sys.exit(main())
