#!/usr/bin/env python3
"""When a swap's present runs (xbox/src/rt/gfx.c, XRC_SWAP_AT_BLOCK): tests/xbox/test_gfx_swap.c over a fake interpreter, with the
switch on and off."""
import sys

import hostcc


def main():
    rc = 0
    for on in (1, 0):
        rc |= hostcc.run(['tests/xbox/test_gfx_swap.c', 'xbox/src/rt/gfx.c'],
                         includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/gfx'],
                         defines=[f'XRC_SWAP_AT_BLOCK={on}'],
                         flags=['-Wno-unused-function', '-include', str(hostcc.ROOT / 'xbox/include/xrc/recomp.h')],
                         args=sys.argv[1:])
    print('test_gfx_swap: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
