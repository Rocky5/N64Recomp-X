#!/usr/bin/env python3
"""The runtime's audio interface (xbox/src/rt/ultra/ai.c) on the host:
builds tests/xbox/test_ai.c against a fake clock, the AI events counted,
with XRC_AI_SPENT on, and checks that the spent-buffer case fails with it
off (-DXRC_AI_SPENT=0)."""
import sys

import hostcc


def build(extra):
    return hostcc.run(['tests/xbox/test_ai.c', 'xbox/src/rt/ultra/ai.c'],
                      includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'],
                      flags=['-Wno-unused-function', '-include', str(hostcc.ROOT / 'xbox/include/xrc/xrc_prelude.h')] + extra)


def main():
    rc = build([])
    print('  with -DXRC_AI_SPENT=0 (expected to fail):', flush=True)
    off = build(['-DXRC_AI_SPENT=0'])
    ok = rc == 0 and off != 0
    print('test_ai: ' + ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
