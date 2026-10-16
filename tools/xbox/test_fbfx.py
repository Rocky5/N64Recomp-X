#!/usr/bin/env python3
"""Framebuffer effects' bookkeeping on the host (docs/renderer.md
"Framebuffer effects"): builds tests/xbox/test_fbfx.c with xbox/src/gfx/fbfx.c
and runs it (slots, drops, stale marks, the stage's quarters, the conversion
into the word-swapped RDRAM image). No ROM."""
import sys

import hostcc


def main():
    rc = hostcc.run(['tests/xbox/test_fbfx.c', 'xbox/src/gfx/fbfx.c'], includes=['xbox/src/gfx'])
    print('test_fbfx: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
