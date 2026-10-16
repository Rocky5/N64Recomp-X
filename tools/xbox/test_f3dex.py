#!/usr/bin/env python3
"""The microcode registry and the F3DEX family on the host
(docs/renderer.md): builds tests/xbox/test_f3dex.c with the real
interpreter (xbox/src/gfx/f3d_interp.c, texconv.c, ucode.c) over a fake
RDRAM and back end, and runs display lists of each GBI (Fast3D, F3DEX,
F3D_OLD, F3DEX2) and the version texts the registry reads. No ROM."""
import sys

import hostcc

ROOT = hostcc.ROOT


def main():
    rc = hostcc.run(['tests/xbox/test_f3dex.c', 'xbox/src/gfx/f3d_interp.c', 'xbox/src/gfx/texconv.c',
                     'xbox/src/gfx/ucode.c', 'xbox/src/gfx/fbfx.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/gfx'],
                    flags=['-DXGFX_FB_READBACK=1', '-Wno-unused-function', '-Wno-unused-variable', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_f3dex: ' + ('FAILED' if rc else 'passed'))
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
