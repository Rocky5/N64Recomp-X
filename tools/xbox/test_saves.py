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


def test_thread_fr():
    """saves.thread_fr: the Status word osCreateThread stores at 0x118 of the
    new context (recomp.h XRC_FR)"""
    bad = []
    if saves.thread_fr(words(0x340DFF03, 0x00000000, 0xAC8D0118)) != 0:   # li t5, 0xFF03 (libultra's)
        bad.append("libultra's SR_IMASK | SR_EXL | SR_IE")
    if saves.thread_fr(words(0x3C0D0400, 0x00000000, 0x35ADFF03, 0xAC8D0118, 0x03E00008)) != 1:   # lui 0x0400 too
        bad.append('FR for every thread')
    if saves.thread_fr(words(0x3C0D0400, 0x35ADFF03, 0xAC8D0118, 0x04A10003, 0x3C01FBFF, 0x3421FFFF,
                             0x01A15824, 0xAC8B0118)) != 2:   # Rare's: the mask without FR for a negative id
        bad.append("Rare's negative id")
    if saves.thread_fr(words(0x3C0D0400, 0x35ADFF03, 0xAC8D0100)) != 0:   # another field
        bad.append('a store to another field')
    for b in bad:
        print(f'  FAIL thread_fr: {b}')
    return len(bad)


def test_os_exception():
    """saves.os_exception: of several copies of the preamble and __osException,
    the one a segment loads where it is linked"""
    from n64rx_lib.analyze.funcs import Segment
    bad = []

    def copy(target):   # lui k0, hi; addiu k0, lo; jr k0; nop; then __osException's start
        hi, lo = (target + 0x8000) >> 16, target & 0xFFFF
        return words(0x3C1A0000 | hi, 0x275A0000 | lo, 0x03400008, 0, 0x3C1A0000 | hi, 0x275A0000 | lo,
                     0xFF410020, 0x401B6000)
    img = bytes(0x100) + copy(0x00200110) + bytes(0x100) + copy(0x00300230)   # at 0x100 and 0x220
    if saves.os_exception(img) != ['-DXRC_OS_EXCEPTION=0x00300230u']:
        bad.append('no segments: the last copy')
    if saves.os_exception(img, [Segment('a', 0, 0x00200000, 0x400)]) != ['-DXRC_OS_EXCEPTION=0x00200110u']:
        bad.append('the copy linked where a segment loads it')
    if saves.os_exception(img, [Segment('a', 0, 0x00400000, 0x400)]) != ['-DXRC_OS_EXCEPTION=0x00300230u']:
        bad.append('no copy linked where it sits: the last')
    for b in bad:
        print(f'  FAIL os_exception: {b}')
    return len(bad)


def test_faulted_thread():
    """saves.faulted_thread: libultra's order and Turok 2's (BadVAddr read before the flags), and the copy
    after `near` when there are several"""
    bad = []
    lib = words(0x3C01800C, 0xAC3A8924, 0x24090001, 0xA7490010, 0x24090002, 0xA7490012, 0x400A4000, 0xAF4A0124)
    t2 = words(0x3C01800C, 0xAC3A97F4, 0x24090001, 0xA7490010, 0x24090002, 0x400A4000, 0xA7490012, 0xAF4A0124)
    if saves.faulted_thread(lib) != ['-DXRC_FAULTED_THREAD=0x800B8924u', '-DXRC_TH_BADVADDR=0x124u']:
        bad.append("libultra's order")
    if saves.faulted_thread(t2) != ['-DXRC_FAULTED_THREAD=0x800B97F4u', '-DXRC_TH_BADVADDR=0x124u']:
        bad.append("BadVAddr before the flags")
    img = bytes(0x40) + lib + bytes(0x40) + t2   # at 0x40 and 0xA0
    if saves.faulted_thread(img)[0] != '-DXRC_FAULTED_THREAD=0x800B97F4u':
        bad.append('no near: the last copy')
    if saves.faulted_thread(img, 0x30)[0] != '-DXRC_FAULTED_THREAD=0x800B8924u':
        bad.append("the copy in os_exception's __osException")
    for b in bad:
        print(f'  FAIL faulted_thread: {b}')
    return len(bad)


def main():
    nbad = test_cont_max() + test_thread_fr() + test_os_exception() + test_faulted_thread()
    rc = hostcc.run(['tests/xbox/test_saves.c', 'xbox/src/rt/ultra/savedev.c', 'xbox/src/rt/ultra/pfs.c',
                     'xbox/src/rt/ultra/pi.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/rt/ultra'],
                    flags=['-Wno-unused-function', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_saves: ' + ('FAILED' if rc or nbad else 'passed'))
    return 1 if rc or nbad else 0


if __name__ == '__main__':
    sys.exit(main())
