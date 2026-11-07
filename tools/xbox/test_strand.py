#!/usr/bin/env python3
"""A hand-written stack switch on strands (xbox/src/rt/strand.c, xrc_strand_switch): tests/xbox/test_strand.c on pthreads.
Contexts started and resumed by the sp they go to, with the registers the switch left, on host threads of their own."""
import sys

import hostcc


def main():
    rc = hostcc.run(['tests/xbox/test_strand.c', 'xbox/src/rt/strand.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'],
                    flags=['-Wno-unused-function', '-pthread', '-include', str(hostcc.ROOT / 'xbox/include/xrc/xrc_prelude.h')],
                    args=sys.argv[1:])
    print('test_strand: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
