#!/usr/bin/env python3
"""The RCP register model (xbox/src/rt/mmio.c) through the interpreter
(xbox/src/rt/interp.c): builds tests/xbox/test_mmio.c with both on the host,
once with 64-bit registers and once with -DXRC_GPR32=1, and runs the checks
(KSEG1 RDRAM, PI DMA, SI's PIF block, AI, VI_ORIGIN, the SP task by DMEM,
MI, the cartridge, RDRAM past its end, an access it does not model)."""
import sys

import hostcc


def main():
    rc = 0
    for defs in ([], ['XRC_GPR32=1']):
        rc |= hostcc.run(['tests/xbox/test_mmio.c', 'xbox/src/rt/interp.c', 'xbox/src/rt/tlb.c', 'xbox/src/rt/mmio.c'],
                         includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'], defines=defs,
                         flags=['-Wno-unused-function', '-include', str(hostcc.ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_mmio: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
