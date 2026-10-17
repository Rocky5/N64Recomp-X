#!/usr/bin/env python3
"""The JPEG decoder task (xbox/src/rt/jpeg.c) on the host: tests/xbox/test_jpeg.c."""
import sys

import hostcc


def main():
    rc = hostcc.run(['tests/xbox/test_jpeg.c', 'xbox/src/rt/jpeg.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'],
                    flags=['-Wno-unused-function', '-include', str(hostcc.ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
