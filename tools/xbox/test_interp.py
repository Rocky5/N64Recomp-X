#!/usr/bin/env python3
"""The MIPS interpreter (xbox/src/rt/interp.c: hand-written routines and
the safety net, PLAN.md 3.14): builds tests/xbox/test_interp.c with it on
the host, once with 64-bit registers and once with -DXRC_GPR32=1, and runs
the instruction-level checks (integer, 64-bit, unaligned, branches and
calls, the learn log's spans, COP1 with the FCR31 rounding mode, traps,
the TLB through tlb.c with its host side recorded)."""
import sys

import hostcc


def main():
    rc = 0
    for defs in ([], ['XRC_GPR32=1']):
        rc |= hostcc.run(['tests/xbox/test_interp.c', 'xbox/src/rt/interp.c', 'xbox/src/rt/tlb.c'],
                         includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'], defines=defs,
                         flags=['-Wno-unused-function', '-include', str(hostcc.ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_interp: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
